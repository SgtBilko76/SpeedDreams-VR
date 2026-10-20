/*
 * vr_stereo.cpp - per-eye camera composition and the frame driver.
 *
 * OpenXR gives us, per eye, a pose in the (recentred) stage space: right-handed,
 * +Y up, -Z forward, metres. Speed Dreams cameras are Z-up with eye/center/up
 * vectors. We build the camera basis (r, u, -f) from the game camera and express
 * the OpenXR eye offset/rotation in that basis, so head tracking works with every
 * camera (cockpit, bonnet, chase, ...).
 *
 * A seated "base" pose (position + yaw) is captured at the first race frame and
 * on VrRequestRecenter(); eye poses are expressed relative to it.
 *
 * Frame structure (see vr_state.h):
 *   VrFrameBegin  -> TBXR_FrameSetup
 *   menus:  GfuiRedraw -> VrMonoBegin (eye 0), GfuiSwapBuffers -> VrPresent (quad layer)
 *   race:   rmRedisplay -> VrRaceFrameBegin / VrRaceEyeBegin+End x2 / VrRaceFrameEnd,
 *           then GfuiSwapBuffers -> VrPresent (projection layer)
 */

#include <math.h>
#include <string.h>
#include <time.h>

extern "C" {
#include "tbxr/VrCommon.h"
}
#undef ALOGV
#undef ALOGE
#include "vr_log.h"

#include <GL/gl.h>

#include "vr_state.h"
#include "vr_keyboard.h"

extern "C" {
int vr_inStereoFrame = 0;
int vr_curEye = 0;
float vr_eyeFov[2][4];
}

/* per-eye pose relative to the seated base, in XR axes */
static float sEyeRot[2][9];   /* row-major 3x3 */
static float sEyePos[2][3];

static bool sHaveBase = false;
static bool sRecenterRequested = true;
static float sBaseYaw = 0.0f;       /* radians, rotation about +Y */
static float sBasePos[3] = {0, 0, 0};

static bool sStereoFrame = false;   /* the race rendered both eyes this frame */
static bool sMonoActive = false;    /* eye 0 is bound for 2D drawing */
static bool sUseScreenLayer = true; /* submit a quad layer (menu) instead of a projection */

static bool sPrevWasStereo = false; /* the previous frame was a race frame */
static int sShaderFlushClock = 0;   /* menu frames since the last shader archive write */
static const int SHADER_FLUSH_FRAMES = 600;   /* ~10 s at 60 fps */

static const float HUD_DISTANCE = 1.5f;   /* metres */
static const float HUD_HEIGHT = 1.2f;     /* metres */

extern "C" void VrInvalidateScreenPose(void);   /* sd_vr.cpp */

extern "C" void VrGetEyeSize(int* w, int* h);
extern "C" void TBXR_ClearFrameBuffer(int width, int height);   /* tbxr/TBXR_Common.c */

/* --------------------------------------------------------------------- math */
static void quatToMat3(const XrQuaternionf& q, float* m)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

static void mat3Mul(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
}

static void mat3MulVec(const float* m, const float* v, float* out)
{
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

static void yawMat3(float yaw, float* m)
{
    float c = cosf(yaw), s = sinf(yaw);
    m[0] = c;  m[1] = 0; m[2] = s;
    m[3] = 0;  m[4] = 1; m[5] = 0;
    m[6] = -s; m[7] = 0; m[8] = c;
}

static float headYaw(const XrQuaternionf& q)
{
    /* yaw of the -Z forward vector projected on the XZ plane */
    float m[9];
    quatToMat3(q, m);
    float fx = -m[2], fz = -m[8];
    return atan2f(-fx, -fz);   /* 0 when looking down -Z */
}

static void normalize3(float* v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void cross3(const float* a, const float* b, float* out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* ------------------------------------------------------------- eye poses */
static void computeEyePoses(void)
{
    /* Views for this frame's predicted display time (fills gAppState.Projections). */
    TBXR_updateProjections();

    const XrPosef& head = gAppState.xfStageFromHead;

    if (sRecenterRequested || !sHaveBase) {
        sBaseYaw = headYaw(head.orientation);
        sBasePos[0] = head.position.x;
        sBasePos[1] = head.position.y;
        sBasePos[2] = head.position.z;
        sHaveBase = true;
        sRecenterRequested = false;
        ALOGI("VR recenter: yaw=%.1f deg pos=(%.2f %.2f %.2f)", sBaseYaw * 180.0f / (float)M_PI,
              sBasePos[0], sBasePos[1], sBasePos[2]);
    }

    float unyaw[9];
    yawMat3(-sBaseYaw, unyaw);

    for (int eye = 0; eye < 2; eye++) {
        XrPosef stageFromEye = XrPosef_Multiply(head, gAppState.Projections[eye].pose);

        float rel[3] = {stageFromEye.position.x - sBasePos[0],
                        stageFromEye.position.y - sBasePos[1],
                        stageFromEye.position.z - sBasePos[2]};
        mat3MulVec(unyaw, rel, sEyePos[eye]);

        float rot[9];
        quatToMat3(stageFromEye.orientation, rot);
        mat3Mul(unyaw, rot, sEyeRot[eye]);

        const XrFovf& f = gAppState.Projections[eye].fov;
        vr_eyeFov[eye][0] = f.angleLeft;
        vr_eyeFov[eye][1] = f.angleRight;
        vr_eyeFov[eye][2] = f.angleUp;
        vr_eyeFov[eye][3] = f.angleDown;
    }
}

extern "C" {

float VrHeadYawRad(void)
{
    return headYaw(gAppState.xfStageFromHead.orientation);
}

void VrRequestRecenter(void)
{
    sRecenterRequested = true;
    VrInvalidateScreenPose();
}

/* The player held the Meta button: the runtime has recentred its spaces (the
 * reference-space-change event, seen by TBXR). The seated pose here is kept
 * relative to where the head was at the last recentre, so take a new one now,
 * as every other VR app does on that button. */
void VrSystemRecentered(void)
{
    ALOGI("VR recenter: requested by the system (Meta button held)");
    VrRequestRecenter();
}

void VrGetEyeSize(int* w, int* h)
{
    *w = (int)gAppState.Width;
    *h = (int)gAppState.Height;
}

/* Place a rectangle in the eye buffer so that it appears at a given direction and
 * distance from the viewer, in both eyes.
 *
 * Anything drawn at fixed pixel coordinates - the rear-view mirror, say - cannot
 * simply use the same rectangle in both eyes. The two frusta are asymmetric and
 * not mirror images of each other, so the same pixel is a different direction in
 * each eye; and with no parallax at all the rectangle would sit at infinity while
 * the cockpit around it is an arm's length away. Either alone is enough to stop
 * the eyes fusing it, which is why it reads as two mirrors.
 *
 * So work the other way round: choose where the thing should be in the world -
 * yaw and pitch from straight ahead, and a distance - and solve for the pixel
 * position in whichever eye is being drawn.
 *
 *   yaw, pitch  radians from straight ahead; pitch positive is up
 *   distance    metres, for the parallax between the eyes
 *   w, h        size of the rectangle in pixels
 *   x, y        bottom-left corner for glViewport
 */
void VrPlaceEyeRect(float yaw, float pitch, float distance, int w, int h, int* x, int* y)
{
    int bw = 0, bh = 0;
    VrGetEyeSize(&bw, &bh);
    if (x) *x = (bw - w) / 2;
    if (y) *y = (bh - h) / 2;

    if (!vr_inStereoFrame || distance <= 0.05f || bw <= 0 || bh <= 0) {
        return;
    }

    const float* f = vr_eyeFov[vr_curEye];

    /* A perspective projection is linear in the tangent of the angle, not in the
     * angle - and these frusta reach past fifty degrees, where the two are not
     * remotely the same. Working in angles left enough error between the eyes to
     * keep the mirror from fusing. */
    const float tanL = tanf(f[0]), tanR = tanf(f[1]);
    const float tanD = tanf(f[3]), tanU = tanf(f[2]);
    if (tanR - tanL < 0.01f || tanU - tanD < 0.01f) {
        return;
    }

    /* Half the interpupillary distance, from the poses the runtime gave us. The
     * left eye sees a point ahead to the right of its own axis, and vice versa. */
    float ipd = 0.063f;                      /* a sane default if the poses are not up yet */
    const float dx = sEyePos[1][0] - sEyePos[0][0];
    const float dy = sEyePos[1][1] - sEyePos[0][1];
    const float dz = sEyePos[1][2] - sEyePos[0][2];
    const float measured = sqrtf(dx * dx + dy * dy + dz * dz);
    if (measured > 0.03f && measured < 0.09f) {
        ipd = measured;
    }

    const float sign = (vr_curEye == 0) ? 1.0f : -1.0f;
    const float tx = tanf(yaw) + sign * (ipd * 0.5f) / distance;
    const float ty = tanf(pitch);

    if (x) *x = (int)((float)bw * (tx - tanL) / (tanR - tanL)) - w / 2;
    if (y) *y = (int)((float)bh * (ty - tanD) / (tanU - tanD)) - h / 2;
}

int VrInMenu(void)
{
    return sUseScreenLayer ? 1 : 0;
}

/* Used by TBXR_submitFrame through VR_UseScreenLayer(). */
bool VrUseScreenLayerFlag(void)
{
    return sUseScreenLayer;
}

void VrApplyEyePose(const float* eye, const float* center, const float* up,
                    float* eyeOut, float* centerOut, float* upOut)
{
    /* Speed Dreams camera basis */
    float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    normalize3(f);
    float r[3];
    cross3(f, up, r);
    normalize3(r);
    float u[3];
    cross3(r, f, u);

    /* M_cam columns: XR +X -> r, XR +Y -> u, XR -Z -> f  (so XR +Z -> -f) */
    const float* R = sEyeRot[vr_curEye];
    const float* T = sEyePos[vr_curEye];

    /* eye offset */
    for (int i = 0; i < 3; i++) {
        eyeOut[i] = eye[i] + T[0] * r[i] + T[1] * u[i] - T[2] * f[i];
    }
    /* forward = R * (0,0,-1) = -third column ; up = R * (0,1,0) = second column */
    float fx = -R[2], fy = -R[5], fz = -R[8];
    float ux = R[1], uy = R[4], uz = R[7];
    for (int i = 0; i < 3; i++) {
        float fw = fx * r[i] + fy * u[i] - fz * f[i];
        float uw = ux * r[i] + uy * u[i] - uz * f[i];
        centerOut[i] = eyeOut[i] + fw;
        upOut[i] = uw;
    }
}

/* For things drawn in camera space after the scene (the rain): the matrix
 * that takes a point given relative to the game's own camera - the seat,
 * looking ahead - into the current eye's space. Without it they are glued to
 * the head and turn with it. GL column-major; multiply it onto the modelview.
 *   eye = R^T * (p - T)   (R, T: this eye's rotation and offset from the seat) */
void VrEyeFromSeatGL(float* m)
{
    const float* R = sEyeRot[vr_curEye];
    const float* T = sEyePos[vr_curEye];

    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++)
            m[col * 4 + row] = R[col * 3 + row];   /* (R^T)[row][col] */
    m[3] = m[7] = m[11] = 0.0f;
    for (int row = 0; row < 3; row++)
        m[12 + row] = -(R[0 * 3 + row] * T[0] + R[1 * 3 + row] * T[1] + R[2 * 3 + row] * T[2]);
    m[15] = 1.0f;
}

void VrApplyEyeRotationOnly(const float* center, const float* up,
                            float* centerOut, float* upOut)
{
    /* camera basis (the background camera sits at the origin) */
    float f[3] = {center[0], center[1], center[2]};
    normalize3(f);
    float r[3];
    cross3(f, up, r);
    normalize3(r);
    float u[3];
    cross3(r, f, u);

    const float* R = sEyeRot[vr_curEye];
    float fx = -R[2], fy = -R[5], fz = -R[8];
    float ux = R[1], uy = R[4], uz = R[7];
    for (int i = 0; i < 3; i++) {
        float fw = fx * r[i] + fy * u[i] - fz * f[i];
        float uw = ux * r[i] + uy * u[i] - uz * f[i];
        centerOut[i] = fw;
        upOut[i] = uw;
    }
}

void VrSetupHudProjection(float left, float right, float bottom, float top)
{
    int w, h;
    VrGetEyeSize(&w, &h);
    glViewport(0, 0, w, h);

    const float* fov = vr_eyeFov[vr_curEye];
    const float n = 0.1f, fa = 100.0f;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(n * tanf(fov[0]), n * tanf(fov[1]), n * tanf(fov[3]), n * tanf(fov[2]), n, fa);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -HUD_DISTANCE);
    float s = HUD_HEIGHT / (top - bottom);
    glScalef(s, s, 1.0f);
    glTranslatef(-(left + right) * 0.5f, -(bottom + top) * 0.5f, 0.0f);
}

/* ------------------------------------------------------ race frame driver */
void VrRaceFrameBegin(void)
{
    computeEyePoses();
    sStereoFrame = false;
}

void VrRaceEyeBegin(int eye)
{
    vr_curEye = eye;
    vr_inStereoFrame = 1;
    if (eye == 0 && sMonoActive) {
        /* The frame opened with eye 0 bound for 2D drawing; the race claims it
         * now. Re-acquiring the same swapchain image would be an error, so just
         * take it over. */
        sMonoActive = false;
        TBXR_ClearFrameBuffer((int)gAppState.Width, (int)gAppState.Height);
    } else {
        TBXR_prepareEyeBuffer(eye);
    }
    int w, h;
    VrGetEyeSize(&w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void VrRaceEyeEnd(int eye)
{
    TBXR_finishEyeBuffer(eye);
    vr_inStereoFrame = 0;
}

void VrRaceFrameEnd(void)
{
    sStereoFrame = true;
    sUseScreenLayer = false;
}

/* ----------------------------------------------------------- frame driver */
void VrFrameBegin(void)
{
    TBXR_FrameSetup();   /* no-op if a frame is already open */

    /* Bind eye 0 straight away, so that whatever draws next lands in it. Screens
     * that paint themselves and call GfuiSwapBuffers directly (the splash screen,
     * the loading screen) never go through GfuiRedraw, and would otherwise draw
     * into no framebuffer at all. */
    VrMonoBegin();
}

void VrMonoBegin(void)
{
    if (sMonoActive || vr_inStereoFrame) {
        return;
    }
    /* A 2D screen is being drawn: it goes on the floating quad layer. */
    sUseScreenLayer = true;
    TBXR_prepareEyeBuffer(0);
    sMonoActive = true;
    int w, h;
    VrGetEyeSize(&w, &h);
    glViewport(0, 0, w, h);
}

/* gl4es persistent shader archive: fpe_writePSA() serialises the program binaries
 * compiled so far to <data>/.gl4es.psa, so the next launch starts with warm
 * shaders instead of stalling on the first frame that needs each one. It is a
 * no-op unless something new was compiled, and it must run on the GL thread and
 * outside a race frame - the write is a file write, and it would otherwise land
 * in the middle of a rendered frame. */
extern "C" void fpe_writePSA(void);

void VrShaderCacheFlush(void)
{
    fpe_writePSA();
}

int VrFrameWasStereo(void)
{
    return sPrevWasStereo ? 1 : 0;
}

void VrPresent(void)
{
    if (vr_inStereoFrame) {
        /* Should not happen: a stereo eye must be closed by VrRaceEyeEnd. */
        TBXR_finishEyeBuffer(vr_curEye);
        vr_inStereoFrame = 0;
    }
    if (sMonoActive) {
        VrKeyboardRender();          /* on-screen keyboard over the menu, same buffer */
        TBXR_finishEyeBuffer(0);
        sMonoActive = false;
    } else if (!sStereoFrame) {
        /* Nothing was drawn at all this frame: keep the compositor fed with the
         * previous content rather than submitting an unrendered swapchain image. */
        TBXR_prepareEyeBuffer(0);
        TBXR_finishEyeBuffer(0);
    }

    TBXR_submitFrame();

    /* Leaving a race for the menus: save any shader compiled while driving. Also
     * flush now and then while in the menus, which is where the first launch
     * compiles most of its shaders - both are skipped unless the archive is
     * actually dirty, and neither can happen inside a race frame. */
    bool wasStereo = sStereoFrame;
    if (sPrevWasStereo && !wasStereo) {
        VrShaderCacheFlush();
        sShaderFlushClock = 0;
    } else if (!wasStereo && ++sShaderFlushClock >= SHADER_FLUSH_FRAMES) {
        VrShaderCacheFlush();
        sShaderFlushClock = 0;
    }
    sPrevWasStereo = wasStereo;

    sStereoFrame = false;
    /* Open the next frame right away, eye 0 bound, so that whatever draws next
     * (the event loop, or a screen that paints and swaps on its own during a
     * blocking load) always has a live frame to draw into.
     *
     * This is where an app that keeps up with the display spends its idle time:
     * xrWaitFrame blocks here until the compositor wants the next frame. Hand the
     * duration to the perf counters, which would otherwise charge it to whatever
     * the event loop was doing - drawing - and make a frame with headroom to
     * spare look exactly saturated. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    TBXR_FrameSetup();
    VrMonoBegin();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    VrPerfWaited((long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL
                 + (t1.tv_nsec - t0.tv_nsec));
}

} /* extern "C" */
