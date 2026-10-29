/* Wii Remote HLE at the KPAD/WPAD level. The Bluetooth stack (WUD/BTE) never
 * runs: KPADInit is skipped and reads are served from host input.
 *
 * NSMBW holds the remote sideways (D-pad on the left), so a physical
 * direction maps to the rotated WPAD bit: left=UP, right=DOWN, up=RIGHT,
 * down=LEFT. Jump is 2, run/fire is 1, spin is a shake. */
#include "runtime.h"
#include "platform.h"

#define WPAD_LEFT  0x0001u
#define WPAD_RIGHT 0x0002u
#define WPAD_DOWN  0x0004u
#define WPAD_UP    0x0008u
#define WPAD_PLUS  0x0010u
#define WPAD_2     0x0100u
#define WPAD_1     0x0200u
#define WPAD_B     0x0400u
#define WPAD_A     0x0800u
#define WPAD_MINUS 0x1000u
#define WPAD_HOME  0x8000u

#define WPAD_ERR_NONE           0
#define WPAD_ERR_NO_CONTROLLER -1
#define WPAD_STATE_SETUP        3

/* KPADStatus (0x84 bytes) */
#define KS_HOLD        0x00
#define KS_TRIG        0x04
#define KS_RELEASE     0x08
#define KS_ACC         0x0C
#define KS_ACC_VALUE   0x18
#define KS_ACC_SPEED   0x1C
#define KS_POS         0x20
#define KS_DIST        0x48
#define KS_DEV_TYPE    0x5C
#define KS_WPAD_ERR    0x5D
#define KS_DPD_VALID   0x5E
#define KS_DATA_FORMAT 0x5F
#define KS_SIZE        0x84

static uint32_t s_prev_hold;
static uint32_t s_frame;

static void stf(uint32_t a, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    st32(a, u);
}

static uint32_t wpad_buttons(uint32_t in)
{
    uint32_t b = 0;
    if (in & PAD_LEFT)  b |= WPAD_UP;
    if (in & PAD_RIGHT) b |= WPAD_DOWN;
    if (in & PAD_UP)    b |= WPAD_RIGHT;
    if (in & PAD_DOWN)  b |= WPAD_LEFT;
    if (in & PAD_JUMP)  b |= WPAD_2;
    if (in & PAD_RUN)   b |= WPAD_1;
    if (in & PAD_PLUS)  b |= WPAD_PLUS;
    if (in & PAD_MINUS) b |= WPAD_MINUS;
    if (in & PAD_A)     b |= WPAD_A;
    if (in & PAD_B)     b |= WPAD_B;
    return b;
}

/* KPADInit / KPADInitEx */
void hle_KPADInit(PPCContext *c) { (void)c; }

/* WPADGetStatus */
void hle_WPADGetStatus(PPCContext *c) { c->r[3] = WPAD_STATE_SETUP; }

/* WPADProbe(chan, u32 *type) */
void hle_WPADProbe(PPCContext *c)
{
    if (c->r[3] == 0) {
        if (c->r[4])
            st32(c->r[4], 0);          /* WPAD_DEV_CORE */
        c->r[3] = WPAD_ERR_NONE;
    } else {
        c->r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
    }
}

/* KPADSetConnectCallback(chan, cb): returns the old callback. The per-channel
 * KPAD block layout (0x80391AC8 + chan*0x578, callback at +0x550) comes from
 * the original function. */
void hle_KPADSetConnectCallback(PPCContext *c)
{
    uint32_t chan = c->r[3], cb = c->r[4];
    uint32_t slot = 0x80391AC8u + chan * 1400 + 1360;
    uint32_t old = ld32(slot);
    st32(slot, cb);
    if (chan == 0 && cb)
        rt_queue_callback(cb, 0, WPAD_ERR_NONE);   /* remote 0 "connects" */
    c->r[3] = old;
}

/* Common body of KPADRead/KPADReadEx: (chan, samples, len, s32 *err, flag) */
void hle_KPADRead(PPCContext *c)
{
    uint32_t chan = c->r[3], out = c->r[4], len = c->r[5], err = c->r[6];
    if (chan != 0 || !len || !out) {
        if (err)
            st32(err, (uint32_t)WPAD_ERR_NO_CONTROLLER);
        c->r[3] = 0;
        return;
    }
    uint32_t in = plat_input();
    uint32_t hold = wpad_buttons(in);
    memset(guest_ptr(out), 0, KS_SIZE);
    st32(out + KS_HOLD, hold);
    st32(out + KS_TRIG, hold & ~s_prev_hold);
    st32(out + KS_RELEASE, s_prev_hold & ~hold);
    s_prev_hold = hold;

    /* remote at rest, face up; a shake swings the X axis hard */
    float ax = 0.0f, ay = 0.0f, az = 1.0f, speed = 0.0f;
    s_frame++;
    if (in & PAD_SHAKE) {
        ax = (s_frame & 2) ? 2.5f : -2.5f;
        speed = 2.5f;
    }
    stf(out + KS_ACC, ax);
    stf(out + KS_ACC + 4, ay);
    stf(out + KS_ACC + 8, az);
    stf(out + KS_ACC_VALUE, 1.0f);
    stf(out + KS_ACC_SPEED, speed);
    stf(out + KS_DIST, 1.0f);
    st8(out + KS_DEV_TYPE, 0);
    st8(out + KS_WPAD_ERR, WPAD_ERR_NONE);
    st8(out + KS_DPD_VALID, 0);
    st8(out + KS_DATA_FORMAT, 1);
    if (err)
        st32(err, WPAD_ERR_NONE);
    c->r[3] = 1;
}

/* WPADControlMotor / WPADEnableMotor and friends */
void hle_ret0(PPCContext *c) { c->r[3] = 0; }
void hle_ret1(PPCContext *c) { c->r[3] = 1; }
