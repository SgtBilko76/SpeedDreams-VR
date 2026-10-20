#ifndef VR_STATE_H
#define VR_STATE_H

/*
 * Stereo rendering state shared between the VR glue (sd_vr.cpp / vr_stereo.cpp)
 * and the patched Speed Dreams code (ssggraph grcam.cpp / grmain.cpp / grscreen.cpp,
 * tgfclient gui.cpp / guieventloop.cpp / guiscreen.cpp, legacymenu racerunningmenus.cpp).
 * Plain C interface: the renderer lives in a separate shared library
 * (libsd_ssggraph.so) that links against the core (libsdvr.so).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 1 while the race is being rendered for one eye (set by VrRaceEyeBegin). */
extern int vr_inStereoFrame;
/* 0 or 1 */
extern int vr_curEye;

/* Eye field of view in radians: [0]=angleLeft (<0), [1]=angleRight, [2]=angleUp, [3]=angleDown (<0) */
extern float vr_eyeFov[2][4];

/* Compose the OpenXR eye pose onto a Speed Dreams camera (Z-up world, metres).
 * eye/center/up are the camera vectors; the outputs are the per-eye camera to
 * feed to grMakeLookAtMat4(). */
void VrApplyEyePose(const float* eye, const float* center, const float* up,
                    float* eyeOut, float* centerOut, float* upOut);

/* Like VrApplyEyePose but with NO positional offset (the eye stays at the origin):
 * only the per-eye head rotation is applied. Used for the skybox/background so it
 * sits at infinity and both eyes see identical sky. */
/* Seat (camera) space -> current eye space, GL column-major 4x4. */
void VrEyeFromSeatGL(float* m);

void VrApplyEyeRotationOnly(const float* center, const float* up,
                            float* centerOut, float* upOut);

/* Set up projection + modelview so that the 2D ortho space [l,r]x[b,t] appears
 * as a head-locked plane in front of the current eye (used for HUD and GUI). */
void VrSetupHudProjection(float left, float right, float bottom, float top);

/* ---- Frame driver ----
 * One OpenXR frame per rendered Speed Dreams frame:
 *   VrFrameBegin()  xrWaitFrame / xrBeginFrame, controllers -> synthetic events.
 *                   Idempotent: called at the top of every event loop iteration
 *                   and again after each present.
 *   VrMonoBegin()   called from GfuiRedraw() when it draws outside a stereo frame:
 *                   binds eye 0's buffer so the 2D menu is drawn into it.
 *   VrPresent()     called from GfuiSwapBuffers(): finishes whatever was drawn and
 *                   submits the frame (projection layer after a stereo race frame,
 *                   floating quad layer for a menu), then opens the next frame. */
void VrFrameBegin(void);
void VrMonoBegin(void);
void VrPresent(void);

/* ---- Race frame driver, called from rmRedisplay() (racerunningmenus.cpp) ---- */
void VrRaceFrameBegin(void);
void VrRaceEyeBegin(int eye);
void VrRaceEyeEnd(int eye);
void VrRaceFrameEnd(void);

/* Recenter the seated position/yaw on the next race frame. */
void VrRequestRecenter(void);

/* Called from TBXR when the runtime recentres (Meta button held). */
void VrSystemRecentered(void);

/* Eye buffer size in pixels. */
void VrGetEyeSize(int* w, int* h);

/* Menu screen / race state */
int  VrInMenu(void);          /* 1 while the floating screen (quad layer) is shown */

/* Where to put a w x h rectangle in the eye buffer so that it appears at the given
 * direction (radians from straight ahead, pitch positive up) and distance, in
 * both eyes. See the definition: the two frusta are asymmetric, so the same
 * pixels are not the same direction in each eye. */
void VrPlaceEyeRect(float yaw, float pitch, float distance, int w, int h,
                    int* x, int* y);

int  VrFrameWasStereo(void);   /* the frame just presented was a stereo race frame */

/* Virtual joystick read used by tgfclient/control.cpp (GfctrlJoyGetCurrentStates).
 * Returns 0 and fills *buttons (bitmask) and axes[] for joystick 'index'
 * (only index 0 exists), -1 otherwise. */
int VrJoyRead(int index, int* buttons, float* axes);

/* ---- Frame time breakdown (vr_perf.c) ----
 * The event loop calls VrPerfMark() at the end of each phase of an iteration;
 * one summary line goes to logcat every few seconds. VR_PERF_DRAW closes a frame. */
enum {
    VR_PERF_WAIT = 0,   /* blocked waiting for the compositor (xrWaitFrame) */
    VR_PERF_EVENT,      /* event dispatch */
    VR_PERF_SIM,        /* recompute + timers */
    VR_PERF_DRAW,       /* predisplay + redisplay, including the OpenXR submit */
    VR_PERF_PHASES
};
void VrPerfMark(int phase);

/* Time spent blocked in the compositor, reported by whoever did the waiting.
 * It is subtracted from the phase it happened in and counted as VR_PERF_WAIT:
 * VrPresent opens the next OpenXR frame at the end of its work, so an app that
 * is comfortably inside its frame budget blocks there, in the middle of what the
 * event loop calls drawing. Without this the frame looks exactly saturated. */
void VrPerfWaited(long long ns);

/* Attribute the draw calls issued since the previous tag to one phase of the
 * scene. Called from cGrScreen::drawScene (ssggraph). */
enum {
    VR_PERF_SCENE_SKY = 0,
    VR_PERF_SCENE_CARS,
    VR_PERF_SCENE_TRACK,   /* the track meshes proper (cgrVtxTableTrackPart) */
    VR_PERF_SCENE_SCENE,   /* the rest of the scene graph: buildings, landscape */
    VR_PERF_SCENE_RAIN,
    VR_PERF_SCENE_HUD,     /* everything outside the tagged phases */
    VR_PERF_SCENES
};
void VrPerfScene(int slot);

/* plib's running total of scene graph leaves drawn (stats_num_leaves), read after
 * the scene: one leaf is one mesh the game asked for, so leaves vs draws says
 * whether the cost is the number of meshes or the way each one is submitted. */
void VrPerfLeaves(int total);

/* One track mesh was drawn (cgrVtxTableTrackPart::draw). */
void VrPerfTrackPart(void);

/* Write the gl4es precompiled shader archive if anything new was compiled.
 * A no-op when the archive is clean; never call it from inside a race frame. */
void VrShaderCacheFlush(void);

#ifdef __cplusplus
}
#endif

#endif
