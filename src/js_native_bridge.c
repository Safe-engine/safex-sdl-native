/*
 * Cầu trung chuyển giữa JS và tầng nền tảng.
 *
 * File này cố tình không biết gì về quảng cáo, thanh toán hay bất cứ tính năng cụ thể nào: JS gửi
 * xuống một cái tên và một chuỗi JSON, Java tự hiểu và trả lời bằng một chuỗi JSON khác. Thêm tính
 * năng native mới thì chỉ sửa phía Java của từng game, không phải sửa engine.
 *
 * Hai thread khác nhau: `callNative` chạy trên thread JS, còn câu trả lời của Java đến trên thread
 * UI của Android. Vì vậy kết quả được xếp vào hàng đợi có mutex rồi `js_native_bridge_pump` rút ra
 * trên thread JS mỗi frame. Không bao giờ gọi JS_Call từ thread Java.
 */
#include "js_native_bridge.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

typedef struct NativeResult
{
  int64_t request_id;
  char *json;
  struct NativeResult *next;
} NativeResult;

static SDL_Mutex *g_result_mutex = NULL;
static NativeResult *g_result_head = NULL;
static NativeResult *g_result_tail = NULL;
static JSValue g_on_native_result = JS_UNDEFINED;
static int64_t g_next_request_id = 0;

static void push_result(int64_t request_id, const char *json)
{
  if (!g_result_mutex)
    return;

  NativeResult *node = (NativeResult *)SDL_calloc(1, sizeof(NativeResult));
  if (!node)
    return;

  node->request_id = request_id;
  node->json = SDL_strdup(json ? json : "{}");

  SDL_LockMutex(g_result_mutex);
  if (!g_result_head)
  {
    g_result_head = node;
    g_result_tail = node;
  }
  else
  {
    g_result_tail->next = node;
    g_result_tail = node;
  }
  SDL_UnlockMutex(g_result_mutex);
}

#ifdef __ANDROID__

static jclass g_bridge_class = NULL;

/*
 * Tìm class Java của cầu nối.
 *
 * Không dùng thẳng `FindClass`: hàm này gọi từ thread JS, mà thread đó gắn vào JVM với class loader
 * của hệ thống — loader ấy chỉ thấy class của Android chứ không thấy class nằm trong APK. Phải mượn
 * ClassLoader của Activity. Đây là cái bẫy JNI kinh điển, bỏ qua là nhận NoClassDefFoundError mà
 * không hiểu vì sao.
 *
 * Kết quả giữ làm global ref, tra đúng một lần cho cả vòng đời app.
 */
static jclass find_bridge_class(JNIEnv *env, jobject activity)
{
  if (g_bridge_class)
    return g_bridge_class;

  jclass activity_class = (*env)->GetObjectClass(env, activity);
  if (!activity_class)
    return NULL;

  jmethodID get_class_loader = (*env)->GetMethodID(
      env, activity_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
  jobject loader = get_class_loader
      ? (*env)->CallObjectMethod(env, activity, get_class_loader)
      : NULL;
  (*env)->DeleteLocalRef(env, activity_class);
  if (!loader)
    return NULL;

  jclass loader_class = (*env)->FindClass(env, "java/lang/ClassLoader");
  jmethodID load_class = loader_class
      ? (*env)->GetMethodID(
            env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")
      : NULL;
  jstring class_name = (*env)->NewStringUTF(env, "com.safeengine.jssdl.NativeBridge");
  jobject found = (load_class && class_name)
      ? (*env)->CallObjectMethod(env, loader, load_class, class_name)
      : NULL;

  if ((*env)->ExceptionCheck(env))
  {
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    found = NULL;
  }

  if (class_name)
    (*env)->DeleteLocalRef(env, class_name);
  if (loader_class)
    (*env)->DeleteLocalRef(env, loader_class);
  (*env)->DeleteLocalRef(env, loader);

  if (!found)
    return NULL;

  g_bridge_class = (jclass)(*env)->NewGlobalRef(env, found);
  (*env)->DeleteLocalRef(env, found);
  return g_bridge_class;
}

static bool dispatch_to_platform(
    int64_t request_id, const char *name, const char *args_json)
{
  JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (!env)
    return false;

  jobject activity = (jobject)SDL_GetAndroidActivity();
  if (!activity)
    return false;

  bool dispatched = false;
  jclass bridge = find_bridge_class(env, activity);
  jmethodID call = bridge
      ? (*env)->GetStaticMethodID(
            env, bridge, "call",
            "(Landroid/app/Activity;JLjava/lang/String;Ljava/lang/String;)V")
      : NULL;

  if (call)
  {
    jstring jname = (*env)->NewStringUTF(env, name);
    jstring jargs = (*env)->NewStringUTF(env, args_json);
    (*env)->CallStaticVoidMethod(
        env, bridge, call, activity, (jlong)request_id, jname, jargs);

    if ((*env)->ExceptionCheck(env))
    {
      (*env)->ExceptionDescribe(env);
      (*env)->ExceptionClear(env);
    }
    else
    {
      dispatched = true;
    }

    if (jname)
      (*env)->DeleteLocalRef(env, jname);
    if (jargs)
      (*env)->DeleteLocalRef(env, jargs);
  }
  else
  {
    SDL_Log("callNative: khong tim thay com.safeengine.jssdl.NativeBridge.call");
  }

  (*env)->DeleteLocalRef(env, activity);
  return dispatched;
}

/* Java gọi vào đây, trên thread UI. Chỉ xếp hàng, tuyệt đối không đụng vào JS. */
JNIEXPORT void JNICALL Java_com_safeengine_jssdl_NativeBridge_nativeResult(
    JNIEnv *env, jclass clazz, jlong request_id, jstring json)
{
  (void)clazz;
  const char *chars = json ? (*env)->GetStringUTFChars(env, json, NULL) : NULL;
  push_result((int64_t)request_id, chars);
  if (chars)
    (*env)->ReleaseStringUTFChars(env, json, chars);
}

#else

static bool dispatch_to_platform(
    int64_t request_id, const char *name, const char *args_json)
{
  (void)request_id;
  (void)name;
  (void)args_json;
  return false;
}

#endif /* __ANDROID__ */

JSValue js_callNative(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1)
    return JS_ThrowTypeError(ctx, "name is required");

  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name)
    return JS_EXCEPTION;

  const char *args_json = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;

  if (!g_result_mutex)
    g_result_mutex = SDL_CreateMutex();

  int64_t request_id = ++g_next_request_id;
  bool dispatched = g_result_mutex
      && dispatch_to_platform(request_id, name, args_json ? args_json : "{}");

  JS_FreeCString(ctx, name);
  if (args_json)
    JS_FreeCString(ctx, args_json);

  /* -1 để JS biết ngay là không ai nhận lệnh, khỏi ngồi chờ một câu trả lời không bao giờ tới. */
  return JS_NewInt64(ctx, dispatched ? request_id : -1);
}

JSValue js_onNativeResult(
    JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
    return JS_ThrowTypeError(ctx, "callback is required");

  JS_FreeValue(ctx, g_on_native_result);
  g_on_native_result = JS_DupValue(ctx, argv[0]);
  return JS_UNDEFINED;
}

void js_native_bridge_pump(JSContext *ctx)
{
  if (!g_result_mutex)
    return;

  for (;;)
  {
    SDL_LockMutex(g_result_mutex);
    NativeResult *node = g_result_head;
    if (node)
    {
      g_result_head = node->next;
      if (!g_result_head)
        g_result_tail = NULL;
    }
    SDL_UnlockMutex(g_result_mutex);

    if (!node)
      break;

    if (JS_IsFunction(ctx, g_on_native_result))
    {
      JSValue args[2];
      args[0] = JS_NewInt64(ctx, node->request_id);
      args[1] = JS_NewString(ctx, node->json);
      JSValue ret = JS_Call(
          ctx, g_on_native_result, JS_UNDEFINED, 2, (JSValueConst *)args);
      JS_FreeValue(ctx, args[0]);
      JS_FreeValue(ctx, args[1]);

      if (JS_IsException(ret))
      {
        JSValue error = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, error);
        SDL_Log("onNativeResult nem loi: %s", text ? text : "(khong ro)");
        if (text)
          JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, error);
      }
      JS_FreeValue(ctx, ret);
    }

    SDL_free(node->json);
    SDL_free(node);
  }
}

void js_native_bridge_shutdown(JSContext *ctx)
{
  JS_FreeValue(ctx, g_on_native_result);
  g_on_native_result = JS_UNDEFINED;

  while (g_result_head)
  {
    NativeResult *node = g_result_head;
    g_result_head = node->next;
    SDL_free(node->json);
    SDL_free(node);
  }
  g_result_tail = NULL;

  if (g_result_mutex)
  {
    SDL_DestroyMutex(g_result_mutex);
    g_result_mutex = NULL;
  }
}
