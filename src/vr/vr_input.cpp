/*
 * vr_input.cpp - Meta Quest Touch controllers -> Speed Dreams input.
 *
 * Driving: a virtual joystick (index 0) consumed by GfctrlJoyGetCurrentStates(),
 * laid out to match the stock preferences.xml "joystick" section:
 *   AXIS0  left thumbstick X             (steer, -1..1)
 *   AXIS1  right index trigger            (throttle 0..1)
 *   AXIS4  left index trigger             (brake / clutch 0..1)
 *   AXIS5/6 right thumbstick              (menu cursor only)
 *   BTN1 right grip (up shift), BTN2 left grip (down shift), BTN3 A (ABS),
 *   BTN5 left thumb click (reverse gear), BTN6 X (ASR)
 *   Y: next camera (F2)
 *   Holding the Meta button on the right controller recenters the seated pose
 *   (the system does it, and the game follows: VrSystemRecentered)
 *
 * Menus: the right-hand aim ray drives the mouse cursor on the floating screen,
 * the right trigger is the left mouse button, A = Return, B / left menu = Escape,
 * the left stick sends arrow keys for lists.
 */

#include <math.h>
#include <string.h>
#include <stdint.h>

extern "C" {
#include "tbxr/VrCommon.h"
}
#undef ALOGV
#undef ALOGE
#include "vr_log.h"

#include <SDL_keycode.h>

#include "vr_input.h"
#include "vr_events.h"
#include "vr_state.h"
#include "vr_keyboard.h"

/* Screen geometry, from tgfclient (GfScrGetSize). Declared here to avoid pulling
 * the whole tgfclient header into the VR layer. */
void GfScrGetSize(int* scrW, int* scrH, int* viewW, int* viewH);

/* The floating menu screen (sd_vr.cpp). */
extern "C" void VR_GetScreenLayerRect(int* x, int* y, int* w, int* h, float* sizeX, float* sizeY);
extern "C" void VrGetScreenQuad(XrPosef* pose, float* sizeX, float* sizeY);

#define VR_JOY_AXES 8

static float sAxes[VR_JOY_AXES];
static int sButtons = 0;

/* menu cursor in window pixels, y down */
static float sCursorX = 512.0f, sCursorY = 384.0f;
static bool sCursorInit = false;

static uint32_t sPrevLeftButtons = 0, sPrevRightButtons = 0;
static float sPrevRightTrigger = 0.0f;
static float sPrevLeftTrigger = 0.0f;
static int sPrevStickDir = 0;

static float deadzone(float v, float dz)
{
    if (fabsf(v) < dz) return 0.0f;
    float s = (fabsf(v) - dz) / (1.0f - dz);
    return v < 0 ? -s : s;
}

/* rotate v by quaternion q */
static void quatRotate(const XrQuaternionf& q, const float* v, float* out)
{
    float qx = q.x, qy = q.y, qz = q.z, qw = q.w;
    float tx = 2.0f * (qy * v[2] - qz * v[1]);
    float ty = 2.0f * (qz * v[0] - qx * v[2]);
    float tz = 2.0f * (qx * v[1] - qy * v[0]);
    out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

/* Cast the right controller's aim ray onto the floating screen. Returns true and
 * the cursor position in window pixels (y down) on a hit. */
static bool aimRayToScreen(float* px, float* py)
{
    const ovrTrackedController& c = rightRemoteTracking_new;
    if (!c.Active) {
        return false;
    }
    XrPosef quad;
    float sx, sy;
    VrGetScreenQuad(&quad, &sx, &sy);

    float u, v;
    XrPosef cyl;
    float radius = 0.0f, angle = 0.0f, height = 0.0f;
    if (VrGetScreenCylinder(&cyl, &radius, &angle, &height)) {
        /* Curved screen: intersect the aim ray with the cylinder rather than with
         * a plane, or the cursor drifts from the ray towards the edges - the very
         * place a wide menu puts its buttons. The viewer is inside the cylinder,
         * so a forward ray always meets the surface exactly once. */
        const float up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, back[3] = {0, 0, 1};
        float Y[3], X[3], F[3];
        quatRotate(cyl.orientation, up, Y);
        quatRotate(cyl.orientation, right, X);
        quatRotate(cyl.orientation, back, F);
        F[0] = -F[0]; F[1] = -F[1]; F[2] = -F[2];   /* panel centre direction */

        const float aim[3] = {0, 0, -1};
        float D[3];
        quatRotate(c.Pose.orientation, aim, D);
        const float O[3] = {c.Pose.position.x - cyl.position.x,
                            c.Pose.position.y - cyl.position.y,
                            c.Pose.position.z - cyl.position.z};

        /* Drop the axis component of both, then it is a 2D ray/circle problem. */
        const float oy = O[0] * Y[0] + O[1] * Y[1] + O[2] * Y[2];
        const float dy = D[0] * Y[0] + D[1] * Y[1] + D[2] * Y[2];
        const float ox = O[0] * X[0] + O[1] * X[1] + O[2] * X[2];
        const float oz = O[0] * F[0] + O[1] * F[1] + O[2] * F[2];
        const float dx = D[0] * X[0] + D[1] * X[1] + D[2] * X[2];
        const float dz = D[0] * F[0] + D[1] * F[1] + D[2] * F[2];

        const float a = dx * dx + dz * dz;
        if (a < 1e-8f) {
            return false;   /* ray runs along the axis */
        }
        const float b = 2.0f * (ox * dx + oz * dz);
        const float cq = ox * ox + oz * oz - radius * radius;
        const float disc = b * b - 4.0f * a * cq;
        if (disc < 0.0f) {
            return false;
        }
        const float t = (-b + sqrtf(disc)) / (2.0f * a);   /* forward hit */
        if (t <= 0.0f) {
            return false;
        }

        const float hx = ox + t * dx, hz = oz + t * dz;
        const float hy = oy + t * dy;
        if (hz <= 0.0f) {
            return false;   /* behind the viewer, on the far side of the cylinder */
        }
        u = 0.5f + atan2f(hx, hz) / angle;
        v = 0.5f + hy / height;
    } else {

    const float fwd[3] = {0, 0, -1}, zAxis[3] = {0, 0, 1}, xAxis[3] = {1, 0, 0}, yAxis[3] = {0, 1, 0};
    float D[3], N[3], X[3], Y[3];
    quatRotate(c.Pose.orientation, fwd, D);
    quatRotate(quad.orientation, zAxis, N);
    quatRotate(quad.orientation, xAxis, X);
    quatRotate(quad.orientation, yAxis, Y);

    float O[3] = {c.Pose.position.x, c.Pose.position.y, c.Pose.position.z};
    float C[3] = {quad.position.x, quad.position.y, quad.position.z};
    float denom = D[0] * N[0] + D[1] * N[1] + D[2] * N[2];
    if (denom > -1e-4f) {
        return false;   /* pointing away from the screen */
    }
    float CO[3] = {C[0] - O[0], C[1] - O[1], C[2] - O[2]};
    float t = (CO[0] * N[0] + CO[1] * N[1] + CO[2] * N[2]) / denom;
    if (t <= 0.0f) {
        return false;
    }
    float P[3] = {O[0] + t * D[0] - C[0], O[1] + t * D[1] - C[1], O[2] + t * D[2] - C[2]};
    u = (P[0] * X[0] + P[1] * X[1] + P[2] * X[2]) / sx + 0.5f;
    v = (P[0] * Y[0] + P[1] * Y[1] + P[2] * Y[2]) / sy + 0.5f;
    }
    /* a little slack outside the screen keeps the cursor reachable at the edges */
    if (u < -0.05f || u > 1.05f || v < -0.05f || v > 1.05f) {
        return false;
    }
    if (u < 0) u = 0; if (u > 1) u = 1;
    if (v < 0) v = 0; if (v > 1) v = 1;

    int rx = 0, ry = 0, rw = 0, rh = 0;
    float d1, d2;
    VR_GetScreenLayerRect(&rx, &ry, &rw, &rh, &d1, &d2);
    *px = rx + u * rw;
    *py = ry + (1.0f - v) * rh;
    return true;
}

extern "C" {

int VrJoyRead(int index, int* buttons, float* axes)
{
    if (index != 0) {
        return -1;
    }
    *buttons = sButtons;
    memcpy(axes, sAxes, sizeof(sAxes));
    return 0;
}

void VrInputUpdate(void)
{
    const ovrInputStateTrackedRemote& L = leftTrackedRemoteState_new;
    const ovrInputStateTrackedRemote& R = rightTrackedRemoteState_new;

    /* ---- virtual joystick ---- */
    sAxes[0] = deadzone(L.Joystick.x, 0.08f);       /* steer */
    sAxes[1] = R.IndexTrigger;                      /* throttle 0..1 */
    sAxes[2] = 0.0f;
    sAxes[3] = 0.0f;
    sAxes[4] = L.IndexTrigger;                      /* brake / clutch 0..1 */
    sAxes[5] = deadzone(R.Joystick.x, 0.08f);       /* menu cursor fallback */
    sAxes[6] = deadzone(R.Joystick.y, 0.08f);
    sAxes[7] = 0.0f;

    /* Button bit index N corresponds to preferences.xml "BTN(N+1)-0". */
    int b = 0;
    if (R.Buttons & xrButton_GripTrigger) b |= 1 << 0;   /* BTN1 = up shift   (right grip) */
    if (L.Buttons & xrButton_GripTrigger) b |= 1 << 1;   /* BTN2 = down shift (left grip)  */
    if (R.Buttons & xrButton_A)           b |= 1 << 2;   /* BTN3 = ABS toggle (A) */
    if (L.Buttons & xrButton_LThumb)      b |= 1 << 4;   /* BTN5 = reverse gear */
    if (L.Buttons & xrButton_X)           b |= 1 << 5;   /* BTN6 = ASR toggle */
    sButtons = b;

    /* Not VrInMenu(): that reads the screen-layer flag, which VrPresent sets back
     * to "menu" when it opens the next frame - so by the time input runs it says
     * menu even in the middle of a race. Harmless while this block only moved a
     * cursor, but it made the left trigger send Escape instead of braking. What
     * the last presented frame actually was is the honest answer. */
    const bool inMenu = !VrFrameWasStereo();

    if (inMenu) {
        /* ---- menu cursor ---- */
        int sw = 0, sh = 0, vw = 0, vh = 0;
        GfScrGetSize(&sw, &sh, &vw, &vh);
        if (!sCursorInit && sw > 0) {
            sCursorX = sw * 0.5f;
            sCursorY = sh * 0.5f;
            sCursorInit = true;
        }

        /* right hand aim ray drives the cursor; the right stick is a fallback */
        float hx, hy;
        if (aimRayToScreen(&hx, &hy)) {
            if (fabsf(hx - sCursorX) >= 1.0f || fabsf(hy - sCursorY) >= 1.0f) {
                sCursorX = hx;
                sCursorY = hy;
                /* Always passive motion (no button held): moving the ray must only
                 * re-highlight, never drag. */
                VrQueueMouseMotion((int)sCursorX, (int)sCursorY, 0);
            }
        }
        float dx = sAxes[5];
        float dy = -sAxes[6];
        if (dx != 0.0f || dy != 0.0f) {
            const float speed = (sh > 0 ? sh : 768) * 0.45f / 72.0f;
            dx *= fabsf(dx);
            dy *= fabsf(dy);
            sCursorX += dx * speed;
            sCursorY += dy * speed;
            if (sCursorX < 0) sCursorX = 0;
            if (sCursorY < 0) sCursorY = 0;
            if (sw > 0 && sCursorX > sw - 1) sCursorX = sw - 1;
            if (sh > 0 && sCursorY > sh - 1) sCursorY = sh - 1;
            VrQueueMouseMotion((int)sCursorX, (int)sCursorY, 0);
        }

        VrKeyboardSetCursor((int)sCursorX, (int)sCursorY);

        bool trig = R.IndexTrigger > 0.5f;
        bool prevTrig = sPrevRightTrigger > 0.5f;
        if (trig && !prevTrig) {
            /* Rising edge only = exactly one click per trigger pull. */
            if (VrKeyboardActive()) {
                if (!VrKeyboardTryClick()) {
                    VrKeyboardActivate();
                }
            } else {
                /* Atomic click: press and release at the same spot in the same
                 * frame, so a click cannot bleed into the screen it opens. */
                VrQueueMouseButton(1, 1, (int)sCursorX, (int)sCursorY);
                VrQueueMouseButton(1, 0, (int)sCursorX, (int)sCursorY);
            }
        }
        sPrevRightTrigger = R.IndexTrigger;

        /* Left trigger goes back, mirroring the right one selecting. The two
         * triggers are what a hand naturally finds without looking, which the
         * face buttons are not; B and the left menu button still work. */
        if (L.IndexTrigger > 0.5f && sPrevLeftTrigger <= 0.5f) {
            VrQueueKey(SDLK_ESCAPE, 1, 0, 0);
            VrQueueKey(SDLK_ESCAPE, 0, 0, 0);
        }
        sPrevLeftTrigger = L.IndexTrigger;
    }

    double nowMs = TBXR_GetTimeInMilliSeconds();
    uint32_t rNew = R.Buttons & ~sPrevRightButtons;
    uint32_t rRel = ~R.Buttons & sPrevRightButtons;
    uint32_t lNew = L.Buttons & ~sPrevLeftButtons;
    uint32_t lRel = ~L.Buttons & sPrevLeftButtons;

    /* Y: next camera (F2). Recentring is the headset's own: hold the Meta
     * button on the right controller (see VrSystemRecentered). */
    if (lNew & xrButton_Y) {
        VrQueueKey(SDLK_F2, 1, 0, 0);
        VrQueueKey(SDLK_F2, 0, 0, 0);
    }

    /* A is both Enter and the ABS toggle (BTN3 above). Enter is what starts a
     * race from the screens that lead into it, and means nothing once you are
     * driving, so both can sit on the same button. */
    if (rNew & xrButton_A) VrQueueKey(SDLK_RETURN, 1, 0, 0);
    if (rRel & xrButton_A) VrQueueKey(SDLK_RETURN, 0, 0, 0);

    /* Right stick click: the rear-view mirror. ssggraph registers it as the "9"
     * key on the race screen rather than as a player control, so send that. */
    if (!inMenu && (rNew & xrButton_RThumb)) {
        VrQueueKey(SDLK_9, 1, 0, 0);
        VrQueueKey(SDLK_9, 0, 0, 0);
    }
    if (rNew & xrButton_B) VrQueueKey(SDLK_ESCAPE, 1, 0, 0);
    if (rRel & xrButton_B) VrQueueKey(SDLK_ESCAPE, 0, 0, 0);
    if (lNew & xrButton_Enter) VrQueueKey(SDLK_ESCAPE, 1, 0, 0);
    if (lRel & xrButton_Enter) VrQueueKey(SDLK_ESCAPE, 0, 0, 0);

    /* Left stick as arrow keys for lists: menu only (in the race it steers).
     * Edge triggered with hysteresis and a minimum repeat interval. */
    if (inMenu) {
        static double lastStickEvent = 0.0;
        int dir = sPrevStickDir;
        float ax = L.Joystick.x, ay = L.Joystick.y;
        if (dir != 0) {
            float held = (dir == 1) ? ay : (dir == -1) ? -ay : (dir == 2) ? ax : -ax;
            if (held < 0.35f) dir = 0;
        }
        if (dir == 0 && nowMs - lastStickEvent > 250.0) {
            if (ay > 0.75f) dir = 1;
            else if (ay < -0.75f) dir = -1;
            else if (ax > 0.75f) dir = 2;
            else if (ax < -0.75f) dir = -2;
            if (dir != 0) lastStickEvent = nowMs;
        }
        if (dir != sPrevStickDir) {
            if (dir != 0 && VrKeyboardActive()) {
                /* keyboard up: the left stick moves the highlighted key */
                int kd = dir == 1 ? 0 : dir == -1 ? 1 : dir == -2 ? 2 : 3;
                VrKeyboardMove(kd);
            } else if (!VrKeyboardActive()) {
                if (sPrevStickDir != 0) {
                    int k = sPrevStickDir == 1 ? SDLK_UP : sPrevStickDir == -1 ? SDLK_DOWN
                          : sPrevStickDir == 2 ? SDLK_RIGHT : SDLK_LEFT;
                    VrQueueKey(k, 0, 0, 0);
                }
                if (dir != 0) {
                    int k = dir == 1 ? SDLK_UP : dir == -1 ? SDLK_DOWN
                          : dir == 2 ? SDLK_RIGHT : SDLK_LEFT;
                    VrQueueKey(k, 1, 0, 0);
                }
            }
            sPrevStickDir = dir;
        }
    }

    sPrevLeftButtons = L.Buttons;
    sPrevRightButtons = R.Buttons;
}

} /* extern "C" */
