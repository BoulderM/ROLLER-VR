/* PROJECTS/ROLLER/vr_input.h
 *
 * VR controller input: OpenXR actions for the Valve Frame, Oculus Touch and KHR
 * simple profiles, read once per frame and written into the VR controller pad
 * (vr_input_pad.h) through vr_input_map_step(). Active only under --vr-menu
 * (the pad exists only then). */
#ifndef VR_INPUT_H
#define VR_INPUT_H

#include <stdbool.h>
#include <openxr/openxr.h>

/* End of vr_init(), after the session exists. Creates the action set, suggests
 * bindings per profile (the Valve one only when valve_frame_enabled), attaches.
 * False = controllers unavailable (logged); keyboard and gamepads still work. */
bool vr_input_xr_init(XrInstance instance, XrSession session, bool valve_frame_enabled);
/* XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: logs each hand's profile and
 * picks the safety gesture (Back+Start on Valve, Start alone otherwise). */
void vr_input_xr_on_profile_changed(void);
/* Once per OpenXR frame, after xrBeginFrame and before main_loop_iteration()
 * (whose UpdateSDL() pumps SDL and so publishes the pad state). */
void vr_input_frame(bool session_focused);
void vr_input_xr_shutdown(void); /* before xrDestroySession */

#endif /* VR_INPUT_H */
