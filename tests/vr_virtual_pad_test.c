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
 *  - trigger axes rest at raw -32768 and map the full raw range to 0..32767. */
#include <SDL3/SDL.h>

#include <stdio.h>

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

int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("FAIL SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    check_raw_attach();
    SDL_Quit();
    if (g_failures) {
        printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
