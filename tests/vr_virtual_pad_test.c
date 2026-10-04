/* tests/vr_virtual_pad_test.c
 *
 * Headset-free check of the SDL fork's virtual gamepad, the transport the VR
 * build uses for Frame controller input (vr_input_pad.c). Needs no GPU and no
 * display (SDL_VIDEODRIVER=dummy), so it runs on the dev LXC. Built and run by
 * `zig build -Dvr test-vr-sdl`. Exit status: 0 = all checks passed, 1 = failure.
 *
 * What it pins down (vr_input_map.c relies on every one of these):
 *  - a GAMEPAD-type virtual joystick gets SDL's automatic gamepad mapping;
 *  - with contiguous masks, raw button i == SDL_GamepadButton i and raw axis
 *    i == SDL_GamepadAxis i (the virtual driver packs indices in mask order);
 *  - stick axes pass raw values through unchanged;
 *  - trigger axes rest at raw -32768 and map the full raw range to 0..32767;
 *  - the VR pad (vr_input_pad.c) fed by vr_input_map_step() reads back as the
 *    intended SDL gamepad elements, and nothing while the session is not focused;
 *  - vr_input_pad_attach() sets SDL_HINT_JOYSTICK_DIRECTINPUT from its argument.
 *    Only the hint value is checked here; the Windows joystick driver that reads it
 *    is not built on Linux, so its effect is verified on Windows. */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "vr_input_pad.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define PAD_BUTTONS (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1) /* 15: SOUTH..DPAD_RIGHT */
#define PAD_AXES    SDL_GAMEPAD_AXIS_COUNT              /* 6 */

static void check_raw_attach(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = PAD_AXES;
    desc.nbuttons = PAD_BUTTONS;
    desc.axis_mask = (1u << PAD_AXES) - 1u;       /* 0x3F */
    desc.button_mask = (1u << PAD_BUTTONS) - 1u;  /* 0x7FFF */
    desc.name = "ROLLER VR Controllers";

    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    if (!id) {
        printf("SDL_AttachVirtualJoystick: %s\n", SDL_GetError());
        return;
    }
    CHECK(SDL_IsJoystickVirtual(id));
    CHECK(SDL_IsGamepad(id));
    char *mapping = SDL_GetGamepadMappingForID(id);
    printf("mapping: %s\n", mapping ? mapping : "(none)");
    SDL_free(mapping);

    SDL_Joystick *joy = SDL_OpenJoystick(id);
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    CHECK(joy != NULL && pad != NULL);
    if (!joy || !pad)
        return;
    CHECK(SDL_GetGamepadJoystick(pad) == joy); /* SDL refcounts one SDL_Joystick per device */
    CHECK(SDL_GetNumJoystickAxes(joy) == PAD_AXES);
    CHECK(SDL_GetNumJoystickButtons(joy) == PAD_BUTTONS);
    CHECK(SDL_GetNumJoystickHats(joy) == 0);
    for (int b = 0; b < PAD_BUTTONS; ++b)
        CHECK(SDL_GamepadHasButton(pad, (SDL_GamepadButton)b));
    CHECK(!SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_MISC1));
    for (int a = 0; a < PAD_AXES; ++a)
        CHECK(SDL_GamepadHasAxis(pad, (SDL_GamepadAxis)a));

    /* Triggers at rest, before anything was written. */
    SDL_UpdateJoysticks();
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == 0);
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == 0);

    /* Raw button b must read as gamepad button b, and only that one. */
    for (int b = 0; b < PAD_BUTTONS; ++b) {
        CHECK(SDL_SetJoystickVirtualButton(joy, b, true));
        SDL_UpdateJoysticks();
        for (int o = 0; o < PAD_BUTTONS; ++o) {
            bool down = SDL_GetGamepadButton(pad, (SDL_GamepadButton)o);
            if (down != (o == b)) {
                printf("FAIL raw button %d -> gamepad button %d reads %d\n", b, o, (int)down);
                g_failures++;
            }
        }
        CHECK(SDL_SetJoystickVirtualButton(joy, b, false));
        SDL_UpdateJoysticks();
    }

    /* Sticks pass through unchanged. */
    static const Sint16 stick_values[] = { -32768, -16384, 0, 16384, 32767 };
    for (int a = SDL_GAMEPAD_AXIS_LEFTX; a <= SDL_GAMEPAD_AXIS_RIGHTY; ++a) {
        for (size_t k = 0; k < SDL_arraysize(stick_values); ++k) {
            CHECK(SDL_SetJoystickVirtualAxis(joy, a, stick_values[k]));
            SDL_UpdateJoysticks();
            Sint16 got = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a);
            if (got != stick_values[k]) {
                printf("FAIL stick axis %d raw %d reads %d\n", a, stick_values[k], got);
                g_failures++;
            }
        }
        CHECK(SDL_SetJoystickVirtualAxis(joy, a, 0));
    }

    /* Triggers: full raw range -> 0..32767. */
    for (int a = SDL_GAMEPAD_AXIS_LEFT_TRIGGER; a <= SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; ++a) {
        CHECK(SDL_SetJoystickVirtualAxis(joy, a, 32767));
        SDL_UpdateJoysticks();
        CHECK(SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a) == 32767);
        CHECK(SDL_SetJoystickVirtualAxis(joy, a, 0));
        SDL_UpdateJoysticks();
        Sint16 mid = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a);
        printf("trigger axis %d raw 0 reads %d\n", a, mid);
        CHECK(mid >= 16382 && mid <= 16385);
        CHECK(SDL_SetJoystickVirtualAxis(joy, a, -32768));
        SDL_UpdateJoysticks();
        CHECK(SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a) == 0);
    }

    SDL_CloseGamepad(pad);
    SDL_CloseJoystick(joy);
    CHECK(SDL_DetachVirtualJoystick(id));
}

/* attach() must put its backend argument into the hint before SDL's joystick init. */
static void check_backend_hint(void)
{
    CHECK(vr_input_pad_attach(false));
    const char *hint = SDL_GetHint(SDL_HINT_JOYSTICK_DIRECTINPUT);
    CHECK(hint != NULL && strcmp(hint, "0") == 0);
    vr_input_pad_detach();

    CHECK(vr_input_pad_attach(true));
    hint = SDL_GetHint(SDL_HINT_JOYSTICK_DIRECTINPUT);
    CHECK(hint != NULL && strcmp(hint, "1") == 0);
    vr_input_pad_detach();
}

/* One frame: map the actions, write the pad, let SDL read it back. dt is 10 ms
 * (never 0: vr_lone_button needs a positive dt to end a tap pulse). */
static void map_frame(VrInputMapState *s, const VrInputActions *a, bool focused)
{
    VrPadState pad;
    vr_input_map_step(s, a, focused, true, 10, &pad);
    vr_input_pad_write(&pad);
    SDL_UpdateJoysticks();
}

static void expect_only_button(SDL_Gamepad *pad, int idx)
{
    for (int b = 0; b < PAD_BUTTONS; ++b) {
        bool down = SDL_GetGamepadButton(pad, (SDL_GamepadButton)b);
        if (down != (b == idx)) {
            printf("FAIL expected only button %d; button %d reads %d\n", idx, b, (int)down);
            g_failures++;
        }
    }
}

static void expect_no_buttons(SDL_Gamepad *pad)
{
    expect_only_button(pad, -1);
}

static void check_mapped_chain(void)
{
    CHECK(vr_input_pad_attach(false));
    CHECK(vr_input_pad_attached());
    SDL_JoystickID id = vr_input_pad_id();
    CHECK(id != 0);
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    CHECK(pad != NULL);
    if (!pad) {
        vr_input_pad_detach();
        return;
    }

    VrInputMapState s;
    VrInputActions a;

    /* Right trigger full: RIGHT_TRIGGER 32767, LEFT_TRIGGER at rest (0). */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.right_trigger = 1.0f;
    map_frame(&s, &a, true);
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == 32767);
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == 0);

    /* Left stick pushed up (OpenXR +y): SDL LEFTY is negative up. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.left_stick_y = 1.0f;
    map_frame(&s, &a, true);
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) == -32767);
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) == 0);

    /* Right stick pushed up (OpenXR +y): SDL RIGHTY is negative up. A sign flip here
     * would read as the stick pushed down. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.right_stick_y = 1.0f;
    map_frame(&s, &a, true);
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) == -32767);
    CHECK(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) == 0);

    /* A is SOUTH and nothing else. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.button_a = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_SOUTH);

    /* D-pad down is DPAD_DOWN and nothing else. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.dpad_down = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);

    /* Left grip squeezed past the press threshold is LEFT_SHOULDER. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.left_squeeze = 1.0f;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);

    /* B, X and Y are EAST, WEST and NORTH. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.button_b = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_EAST);

    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.button_x = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_WEST);

    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.button_y = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_NORTH);

    /* Right grip squeezed past the press threshold is RIGHT_SHOULDER (the gear-up bumper). */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.right_squeeze = 1.0f;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);

    /* Stick clicks are LEFT_STICK and RIGHT_STICK. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.left_stick_click = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);

    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.right_stick_click = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);

    /* D-pad up and sideways are DPAD_UP, DPAD_LEFT and DPAD_RIGHT. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.dpad_up = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);

    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.dpad_left = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);

    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.dpad_right = true;
    map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

    /* Menu is START and View is BACK, both held back for VR_COMBO_GRACE_MS (250 ms) in
     * case the partner follows. Holding 100 frames of 10 ms (1 s) lets them through,
     * and stays under the 2 s safety hold. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.menu = true;
    for (int f = 0; f < 100; ++f)
        map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_START);

    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.view = true;
    for (int f = 0; f < 100; ++f)
        map_frame(&s, &a, true);
    expect_only_button(pad, SDL_GAMEPAD_BUTTON_BACK);

    /* Back and Start together is the safety combo: neither reaches the game, and GUIDE
     * (the system button) is never driven by the map. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.menu = a.view = true;
    map_frame(&s, &a, true);
    expect_no_buttons(pad);

    /* Not focused with everything pressed: every axis 0 and no button. */
    vr_input_map_init(&s);
    memset(&a, 0, sizeof(a));
    a.left_stick_x = a.left_stick_y = a.right_stick_x = a.right_stick_y = 1.0f;
    a.left_trigger = a.right_trigger = 1.0f;
    a.left_squeeze = a.right_squeeze = 1.0f;
    a.button_a = a.button_b = a.button_x = a.button_y = true;
    a.menu = a.view = true;
    a.left_shoulder = a.right_shoulder = true;
    a.left_stick_click = a.right_stick_click = true;
    a.dpad_up = a.dpad_down = a.dpad_left = a.dpad_right = true;
    a.select = true;
    map_frame(&s, &a, false);
    for (int ax = 0; ax < PAD_AXES; ++ax) {
        Sint16 v = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)ax);
        if (v != 0) {
            printf("FAIL unfocused axis %d reads %d\n", ax, v);
            g_failures++;
        }
    }
    expect_no_buttons(pad);

    SDL_CloseGamepad(pad);
    vr_input_pad_detach();
    CHECK(!vr_input_pad_attached());
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0); /* main()'s own reference survives */
}

int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("FAIL SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    check_raw_attach();
    check_backend_hint();
    check_mapped_chain();
    SDL_Quit();
    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
