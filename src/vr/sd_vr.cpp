/*
 * sd_vr.cpp - Android/OpenXR entry point for Speed Dreams VR.
 *
 * JNI glue (activity + surface lifecycle), the app thread that owns the OpenXR
 * session and the GL context, and the VR_* callbacks the Team Beef framework
 * (src/vr/tbxr) expects the game to provide.
 *
 * Speed Dreams' own main() is compiled into this library as SdMain() and called
 * from the app thread with a synthetic command line pointing at the on-device
 * data and user-settings directories.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>

#include <dlfcn.h>

#include <EGL/egl.h>
#include <GL/gl.h>
#include <gl4esinit.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

extern "C" {
#include "tbxr/VrCommon.h"
}
#undef ALOGV
#undef ALOGE
#include "vr_log.h"

#include "vr_paths.h"
#include "vr_input.h"
#include "vr_audio.h"
#include "vr_config.h"
#include "vr_state.h"

/* tgfclient (C++ linkage): the menu view rectangle inside the eye buffer. */
void GfScrGetSize(int* scrW, int* scrH, int* viewW, int* viewH);

/* Speed Dreams' main(), renamed at compile time (see CMakeLists.txt). */
extern int SdMain(int argc, char* argv[]);

/* ------------------------------------------------------------------------- */
/* Globals the TBXR framework expects the game to provide                    */
/* ------------------------------------------------------------------------- */
extern "C" {
ovrInputStateTrackedRemote leftTrackedRemoteState_old;
ovrInputStateTrackedRemote leftTrackedRemoteState_new;
ovrTrackedController leftRemoteTracking_new;
ovrInputStateTrackedRemote rightTrackedRemoteState_old;
ovrInputStateTrackedRemote rightTrackedRemoteState_new;
ovrTrackedController rightRemoteTracking_new;

float playerYaw = 0.0f;
vec3_t hmdorientation = {0, 0, 0};
}

static char gBaseDir[1024] = "/sdcard/SpeedDreamsVR";
static float gHmdPosition[3] = {0, 0, 0};
static bool gPlayerYawSet = false;

/* vr_stereo.cpp */
extern "C" bool VrUseScreenLayerFlag(void);
extern "C" float VrHeadYawRad(void);
/* TBXR_Common.c tunables */
extern "C" float SS_MULTIPLIER;
extern "C" int REFRESH;
extern "C" int NUM_MULTI_SAMPLES;
extern "C" int TBXR_FoveationLevel;
static float gScreenDistance = 2.5f;

/* ------------------------------------------------------------------------- */
/* VR_* callbacks (game side of the TBXR contract)                           */
/* ------------------------------------------------------------------------- */
extern "C" {

void VR_FrameSetup() {}

bool VR_UseScreenLayer()
{
    return VrUseScreenLayerFlag();
}

float VR_GetScreenLayerDistance()
{
    return gScreenDistance;
}

/* The 2D GUI is drawn into a 4:3 view centred in eye 0's buffer (see the ANDROID
 * branch of GfScrInit); show exactly that rectangle on the floating screen. */
void VR_GetScreenLayerRect(int* x, int* y, int* w, int* h, float* sizeX, float* sizeY)
{
    int sw = 0, sh = 0, vw = 0, vh = 0;
    GfScrGetSize(&sw, &sh, &vw, &vh);
    if (vw <= 0 || vh <= 0) {
        return;
    }
    *x = (sw - vw) / 2;
    *y = (sh - vh) / 2;
    *w = vw;
    *h = vh;
    *sizeY = 1.8f;
    *sizeX = 1.8f * (float)vw / (float)vh;
}

/* Pose of the floating screen in the current (stage) space. It is anchored in
 * front of the head when first needed and after a recenter, so it stays put
 * while the user points at it. */
static XrPosef gScreenPose;
static bool gScreenPoseValid = false;

void VrInvalidateScreenPose(void)
{
    gScreenPoseValid = false;
}

void VrGetScreenQuad(XrPosef* pose, float* sizeX, float* sizeY)
{
    int x = 0, y = 0, w = 0, h = 0;
    float sx = 2.4f, sy = 1.8f;
    VR_GetScreenLayerRect(&x, &y, &w, &h, &sx, &sy);
    if (!gScreenPoseValid) {
        const float yaw = VrHeadYawRad();
        const float d = VR_GetScreenLayerDistance();
        const XrVector3f hp = gAppState.xfStageFromHead.position;
        gScreenPose.position.x = hp.x - sinf(yaw) * d;
        gScreenPose.position.y = hp.y - 0.15f;
        gScreenPose.position.z = hp.z - cosf(yaw) * d;
        gScreenPose.orientation.x = 0.0f;
        gScreenPose.orientation.y = sinf(yaw * 0.5f);
        gScreenPose.orientation.z = 0.0f;
        gScreenPose.orientation.w = cosf(yaw * 0.5f);
        gScreenPoseValid = true;
        ALOGI("Screen anchored at (%.2f %.2f %.2f) yaw %.1f deg", gScreenPose.position.x,
              gScreenPose.position.y, gScreenPose.position.z, yaw * 180.0f / (float)M_PI);
    }
    *pose = gScreenPose;
    *sizeX = sx;
    *sizeY = sy;
}

/* The same screen expressed as a cylinder wrapped around the viewer, which is how
 * it is composited when the runtime has XR_KHR_composition_layer_cylinder.
 *
 * The quad form has its pose on the panel; the cylinder form has it on the axis,
 * which is the head - so the surface sits the same distance away at every angle
 * and is square-on wherever you look. That is what makes a wide menu comfortable:
 * a flat panel this size has its far edges both further away and turned away.
 *
 *   pose          on the axis, +Z pointing back at the viewer (as for the quad)
 *   radius        distance to the surface
 *   centralAngle  angular width, so that radius * centralAngle is the arc width
 *   height        panel height in metres
 *
 * Returns 0 when the curved layer is not in use, so callers fall back to the quad.
 */
extern "C" bool TBXR_HasCylinderLayer;

int VrGetScreenCylinder(XrPosef* pose, float* radius, float* centralAngle, float* height)
{
    if (!TBXR_HasCylinderLayer) {
        return 0;
    }

    XrPosef quad;
    float sx = 0.0f, sy = 0.0f;
    VrGetScreenQuad(&quad, &sx, &sy);

    const float r = VR_GetScreenLayerDistance();

    /* Push the pose from the panel back to the axis, along the panel normal (+Z of
     * the orientation, which points at the viewer). */
    const XrQuaternionf q = quad.orientation;
    const float zx = 2.0f * (q.x * q.z + q.w * q.y);
    const float zy = 2.0f * (q.y * q.z - q.w * q.x);
    const float zz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);

    *pose = quad;
    pose->position.x += zx * r;
    pose->position.y += zy * r;
    pose->position.z += zz * r;

    *radius = r;
    *centralAngle = sx / r;
    *height = sy;
    return 1;
}

bool VR_GetVRProjection(int eye, float zNear, float zFar, float* projection)
{
    XrMatrix4x4f_CreateProjectionFov(&gAppState.ProjectionMatrices[eye], GRAPHICS_OPENGL_ES,
                                     gAppState.Projections[eye].fov, zNear, zFar);
    memcpy(projection, gAppState.ProjectionMatrices[eye].m, 16 * sizeof(float));
    return true;
}

void VR_HandleControllerInput()
{
    TBXR_UpdateControllers();
    VrInputUpdate();
}

void VR_SetHMDOrientation(float pitch, float yaw, float roll)
{
    hmdorientation[0] = pitch;
    hmdorientation[1] = yaw;
    hmdorientation[2] = roll;
    if (!gPlayerYawSet) {
        playerYaw = yaw;
        gPlayerYawSet = true;
    }
}

void VR_SetHMDPosition(float x, float y, float z)
{
    gHmdPosition[0] = x;
    gHmdPosition[1] = y;
    gHmdPosition[2] = z;
}

void VR_HapticEvent(const char* event, int position, int flags, int intensity, float angle, float yHeight) {}
void VR_HapticUpdateEvent(const char* event, int intensity, float angle) {}
void VR_HapticEndFrame() {}
void VR_HapticStopEvent(const char* event) {}
void VR_HapticEnable() {}
void VR_HapticDisable() {}

} /* extern "C" */

/* ------------------------------------------------------------------------- */
/* gl4es bring-up                                                            */
/* ------------------------------------------------------------------------- */
static void gl4esGetMainFbSize(int* width, int* height)
{
    *width = (int)gAppState.Width;
    *height = (int)gAppState.Height;
}

static void* gl4esGetProcAddress(const char* name)
{
    return (void*)eglGetProcAddress(name);
}

static void initGl4es(void)
{
    setenv("LIBGL_ES", "2", 1);
    setenv("LIBGL_GL", "21", 1);
    setenv("LIBGL_NOBANNER", "0", 1);
    setenv("LIBGL_MIPMAP", "3", 1);
    setenv("LIBGL_NPOT", "1", 1);
    setenv("LIBGL_FB", "0", 1);
    setenv("LIBGL_NOERROR", "1", 1);
    /* NOTE: do NOT set LIBGL_NOTEST. It skips gl4es' hardware-capability probe,
     * which also skips program-binary detection and disables the precompiled
     * shader archive (PSA) we rely on for a warm start. */
    setenv("LIBGL_PSA_FOLDER", gBaseDir, 1);
    set_getprocaddress(gl4esGetProcAddress);
    set_getmainfbsize(gl4esGetMainFbSize);
    initialize_gl4es();
    ALOGI("gl4es initialised: GL_VERSION=%s", (const char*)glGetString(GL_VERSION));
    ALOGI("gl4es GL_EXTENSIONS=%s", (const char*)glGetString(GL_EXTENSIONS));
}

/* ------------------------------------------------------------------------- */
/* JNI callback into the activity                                            */
/* ------------------------------------------------------------------------- */
static JavaVM* jVM = NULL;
static jobject jniCallbackObj = 0;
static jmethodID android_shutdown = 0;

static void jni_shutdown()
{
    ALOGV("Calling: jni_shutdown");
    if (!jVM || !jniCallbackObj || !android_shutdown) {
        return;
    }
    JNIEnv* env = NULL;
    if (jVM->GetEnv((void**)&env, JNI_VERSION_1_4) < 0) {
        jVM->AttachCurrentThread(&env, NULL);
    }
    env->CallVoidMethod(jniCallbackObj, android_shutdown);
}

extern "C" void VR_Shutdown()
{
    jni_shutdown();
}

/* ------------------------------------------------------------------------- */
/* App thread                                                                */
/* ------------------------------------------------------------------------- */
extern "C" void* AppThreadFunction(void* parm)
{
    gAppThread = (ovrAppThread*)parm;

    java.Vm = gAppThread->JavaVm;
    java.Vm->AttachCurrentThread(&java.Env, NULL);
    java.ActivityObject = gAppThread->ActivityObject;

    // AttachCurrentThread resets the thread name.
    prctl(PR_SET_NAME, (long)"SDVR_App", 0, 0, 0);

    gAppState.MainThreadTid = gettid();

    VrLogRedirectStdio();

    // User tunables (refresh rate, eye buffer scale) from <base>/vr.cfg
    {
        char cfg[1024];
        snprintf(cfg, sizeof(cfg), "%s/vr.cfg", gBaseDir);
        VrConfigLoad(cfg);
        REFRESH = VrConfigGetInt("refresh", 72);
        SS_MULTIPLIER = VrConfigGetFloat("supersampling", 1.0f);
        gScreenDistance = VrConfigGetFloat("screen_distance", 2.5f);
        if (SS_MULTIPLIER < 0.3f) SS_MULTIPLIER = 0.3f;
        if (SS_MULTIPLIER > 2.0f) SS_MULTIPLIER = 2.0f;

        /* Multisampling for the eye buffers. The framework asks for it through
         * GL_EXT_multisampled_render_to_texture, so the resolve happens in tile
         * memory and never costs a full-size buffer read back; on this port it is
         * cheap, though not free now that the graphics chip is the limit in a
         * race. 1 disables it. */
        NUM_MULTI_SAMPLES = VrConfigGetInt("msaa", 4);
        if (NUM_MULTI_SAMPLES < 1) NUM_MULTI_SAMPLES = 1;
        if (NUM_MULTI_SAMPLES > 8) NUM_MULTI_SAMPLES = 8;

        TBXR_FoveationLevel = VrConfigGetInt("foveation", 2);
        if (TBXR_FoveationLevel < 0) TBXR_FoveationLevel = 0;
        if (TBXR_FoveationLevel > 3) TBXR_FoveationLevel = 3;

        ALOGI("VR settings: refresh=%d Hz supersampling=%.2f msaa=%dx foveation=%d screen_distance=%.1f",
              REFRESH, SS_MULTIPLIER, NUM_MULTI_SAMPLES, TBXR_FoveationLevel, gScreenDistance);
    }

    /* Java's System.loadLibrary() loads this library into the app's local scope.
     * The dlopen-ed game modules each carry their own weak copy of the C++
     * type_info for the module interfaces (IUserInterface, IRaceEngine, ...) and
     * would bind to it rather than to ours, which makes the dynamic_cast in
     * GfModule::getInterface() fail. Re-opening ourselves with RTLD_GLOBAL puts
     * this library in the global lookup scope, so every module resolves those
     * symbols here instead: exactly what -Wl,-E does for the desktop executable. */
    {
        void* self = dlopen("libsdvr.so", RTLD_NOW | RTLD_GLOBAL);
        ALOGI("Core library promoted to the global symbol scope: %s",
              self ? "ok" : dlerror());
    }

    TBXR_InitialiseOpenXR();
    TBXR_EnterVR();          // EGL context is current from here on
    initGl4es();             // must precede any GL call (including the eye FBOs)
    TBXR_InitRenderer();
    TBXR_InitActions();

    ALOGI("OpenXR initialised; eye buffer %d x %d", (int)gAppState.Width, (int)gAppState.Height);

    if (VrPathsInit(gBaseDir) != 0) {
        ALOGE("Data directory not usable; push the game data to %s/data", gBaseDir);
    }
    VrAudioInit();

    TBXR_WaitForSessionActive();
    ALOGI("OpenXR session active");

    // Speed Dreams' main(), with the on-device directories on the command line.
    {
        static char arg0[] = "speed-dreams-2";
        static char optDD[] = "-dd";  static char valDD[1024];
        static char optLC[] = "-lc";  static char valLC[1024];
        static char optLD[] = "-ld";  static char valLD[1024];
        static char optBD[] = "-bd";  static char valBD[1024];
        snprintf(valDD, sizeof(valDD), "%s", VrDataDir());
        snprintf(valLC, sizeof(valLC), "%s", VrLocalDir());
        snprintf(valLD, sizeof(valLD), "%s", VrDataDir());
        snprintf(valBD, sizeof(valBD), "%s", VrBaseDir());
        static char optSR[] = "-s";
        static char valSR[128];
        char* argv[12] = {arg0, optDD, valDD, optLC, valLC, optLD, valLD, optBD, valBD, NULL, NULL, NULL};
        int argc = 9;

        /* vr.cfg may name a race to start directly, skipping the menus. This is
         * how a race gets tested without pointing at menu items by hand:
         *   startrace = practice   */
        const char* race = VrConfigGetStr("startrace", NULL);
        if (race && race[0]) {
            snprintf(valSR, sizeof(valSR), "%s", race);
            argv[argc++] = optSR;
            argv[argc++] = valSR;
            ALOGI("Starting race directly: %s", valSR);
        }
        // There is no SDL_main on this platform: tell SDL the app is ready,
        // otherwise SDL_Init() refuses to start.
        SDL_SetMainReady();

        ALOGI("Starting Speed Dreams: data=%s local=%s", valDD, valLC);
        SdMain(argc, argv);
    }
    ALOGI("Main loop finished, shutting down");

    TBXR_LeaveVR();
    VR_Shutdown();
    exit(0);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* JNI entry points                                                          */
/* ------------------------------------------------------------------------- */
extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv* env;
    jVM = vm;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_4) != JNI_OK) {
        ALOGE("Failed JNI_OnLoad");
        return -1;
    }
    return JNI_VERSION_1_4;
}

JNIEXPORT jlong JNICALL Java_com_speeddreamsvr_SDVRLib_onCreate(JNIEnv* env, jclass activityClass,
                                                                jobject activity, jstring dataDir)
{
    ALOGV("    SDVRLib::onCreate()");

    const char* dir = env->GetStringUTFChars(dataDir, NULL);
    if (dir && dir[0]) {
        snprintf(gBaseDir, sizeof(gBaseDir), "%s", dir);
    }
    env->ReleaseStringUTFChars(dataDir, dir);
    ALOGI("Base dir: %s", gBaseDir);
    setenv("SDVR_DATA_DIR", gBaseDir, 1);
    /* SDL: no window system, audio through the native OpenSL ES backend. */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "openslES", 1);

    ovrAppThread* appThread = (ovrAppThread*)malloc(sizeof(ovrAppThread));
    ovrAppThread_Create(appThread, env, activity, activityClass);

    surfaceMessageQueue_Enable(&appThread->MessageQueue, true);
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_CREATE, MQ_WAIT_PROCESSED);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);

    return (jlong)((size_t)appThread);
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onStart(JNIEnv* env, jobject obj, jlong handle, jobject obj1)
{
    ALOGV("    SDVRLib::onStart()");

    jniCallbackObj = (jobject)env->NewGlobalRef(obj1);
    jclass callbackClass = env->GetObjectClass(jniCallbackObj);
    android_shutdown = env->GetMethodID(callbackClass, "shutdown", "()V");

    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_START, MQ_WAIT_PROCESSED);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onResume(JNIEnv* env, jobject obj, jlong handle)
{
    ALOGV("    SDVRLib::onResume()");
    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_RESUME, MQ_WAIT_PROCESSED);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onPause(JNIEnv* env, jobject obj, jlong handle)
{
    ALOGV("    SDVRLib::onPause()");
    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_PAUSE, MQ_WAIT_PROCESSED);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onStop(JNIEnv* env, jobject obj, jlong handle)
{
    ALOGV("    SDVRLib::onStop()");
    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_STOP, MQ_WAIT_PROCESSED);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onDestroy(JNIEnv* env, jobject obj, jlong handle)
{
    ALOGV("    SDVRLib::onDestroy()");
    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_DESTROY, MQ_WAIT_PROCESSED);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
    surfaceMessageQueue_Enable(&appThread->MessageQueue, false);

    ovrAppThread_Destroy(appThread, env);
    free(appThread);
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onSurfaceCreated(JNIEnv* env, jobject obj, jlong handle, jobject surface)
{
    ALOGV("    SDVRLib::onSurfaceCreated()");
    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);

    ANativeWindow* newNativeWindow = ANativeWindow_fromSurface(env, surface);
    if (ANativeWindow_getWidth(newNativeWindow) < ANativeWindow_getHeight(newNativeWindow)) {
        ALOGE("        Surface not in landscape mode!");
    }

    appThread->NativeWindow = newNativeWindow;
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_SURFACE_CREATED, MQ_WAIT_PROCESSED);
    surfaceMessage_SetPointerParm(&message, 0, appThread->NativeWindow);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onSurfaceChanged(JNIEnv* env, jobject obj, jlong handle, jobject surface)
{
    ALOGV("    SDVRLib::onSurfaceChanged()");
    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);

    ANativeWindow* newNativeWindow = ANativeWindow_fromSurface(env, surface);
    if (ANativeWindow_getWidth(newNativeWindow) < ANativeWindow_getHeight(newNativeWindow)) {
        ALOGE("        Surface not in landscape mode!");
    }

    if (newNativeWindow != appThread->NativeWindow) {
        if (appThread->NativeWindow != NULL) {
            srufaceMessage message;
            surfaceMessage_Init(&message, MESSAGE_ON_SURFACE_DESTROYED, MQ_WAIT_PROCESSED);
            surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
            ANativeWindow_release(appThread->NativeWindow);
            appThread->NativeWindow = NULL;
        }
        if (newNativeWindow != NULL) {
            appThread->NativeWindow = newNativeWindow;
            srufaceMessage message;
            surfaceMessage_Init(&message, MESSAGE_ON_SURFACE_CREATED, MQ_WAIT_PROCESSED);
            surfaceMessage_SetPointerParm(&message, 0, appThread->NativeWindow);
            surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
        }
    } else if (newNativeWindow != NULL) {
        ANativeWindow_release(newNativeWindow);
    }
}

JNIEXPORT void JNICALL Java_com_speeddreamsvr_SDVRLib_onSurfaceDestroyed(JNIEnv* env, jobject obj, jlong handle)
{
    ALOGV("    SDVRLib::onSurfaceDestroyed()");
    ovrAppThread* appThread = (ovrAppThread*)((size_t)handle);
    srufaceMessage message;
    surfaceMessage_Init(&message, MESSAGE_ON_SURFACE_DESTROYED, MQ_WAIT_PROCESSED);
    surfaceMessageQueue_PostMessage(&appThread->MessageQueue, &message);
    if (appThread->NativeWindow) {
        ANativeWindow_release(appThread->NativeWindow);
    }
    appThread->NativeWindow = NULL;
}

} /* extern "C" */
