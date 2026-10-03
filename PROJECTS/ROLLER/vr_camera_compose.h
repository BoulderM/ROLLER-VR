#ifndef VR_CAMERA_COMPOSE_H
#define VR_CAMERA_COMPOSE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Low-level 3x3 matrix helpers --------------------------------------
 * Naive [row][col] array reads throughout (m[r][c]), matching
 * vr_runtime.c's existing quat_to_mat3()/mat3_transpose()/mat3_flip_row2()
 * convention exactly -- this is the ONE shared copy of this math; Task 3
 * deletes vr_runtime.c's own duplicate copies and includes this header
 * instead. */
void vr_quat_to_mat3(float x, float y, float z, float w, float m[3][3]);
void vr_mat3_transpose(const float src[3][3], float dst[3][3]);
void vr_mat3_mul(const float a[3][3], const float b[3][3], float out[3][3]);
/* S*src, S=diag(1,1,-1): negates row 2 only. Converts between OpenXR's
 * local/reference-space axis convention (+X right, +Y up, -Z forward) and
 * the game's own convention (+X right, +Y up, +Z forward) -- see
 * vr_camera_view_basis()'s own derivation comment below for the full
 * justification; this is the exact same S. */
void vr_mat3_flip_row2(const float src[3][3], float dst[3][3]);

/* --- Head-pose composition ---------------------------------------------
 * base_rot: cockpit local-to-world rotation, [row][col] -- i.e. vk1..vk9
 *   read naively as {{vk1,vk2,vk3},{vk4,vk5,vk6},{vk7,vk8,vk9}} (the SAME
 *   convention calculateview()/vk1..vk9 use throughout the game).
 * base_pos: {viewx, viewy, viewz} before composition.
 * qx,qy,qz,qw / px,py,pz: one eye's OpenXR pose (xr_view->pose) -- reference-
 *   space axes, position in meters.
 * meters_to_world: world-units-per-meter scale (vr_resolve_meters_to_world()).
 * out_rot/out_pos: the composed local-to-world rotation (same [row][col]
 *   convention as base_rot -- write straight into vk1..vk9) and world
 *   position (write straight into viewx/viewy/viewz). out_rot/out_pos may
 *   alias base_rot/base_pos.
 *
 * Derivation: let V = base_rot (cockpit local-to-world), H =
 * vr_quat_to_mat3(q) (head local-to-reference, OpenXR axes), S =
 * diag(1,1,-1). The head's orientation re-expressed in the game's own
 * +Z-forward convention is H_game = S*H*S (a similarity transform under the
 * axis flip -- S is its own inverse). The composed cockpit-plus-head
 * local-to-world rotation is out_rot = V * H_game = V*S*H*S. This is the
 * SAME relationship the engine's now-retired build_mvp_vr_eye() used for
 * its own combined_rot, back when it composed the live head pose itself
 * (its own derivation comment proved combined_rot = Hᵗ*S*Vᵗ = (V*S*H)ᵗ);
 * the two are related by out_rot = (combined_rot)ᵗ * S, confirmed algebraically by
 * substitution (S*outᵗ = S*Sᵗ*Hᵗ*Vᵗ*Sᵗ = Hᵗ*S*Vᵗ = combined_rot, using
 * S=Sᵗ and S*S=I). This cross-check is exercised in
 * tests/vr_camera_compose_test.c. The world position offset follows the
 * same forward chain (not inverted, since this composes eye-local into
 * world): head_pos_world = V * S * p_xr * meters_to_world; out_pos =
 * base_pos + head_pos_world. */
void vr_camera_compose(const float base_rot[3][3], const float base_pos[3],
                        float qx, float qy, float qz, float qw,
                        float px, float py, float pz,
                        float meters_to_world,
                        float out_rot[3][3], float out_pos[3]);

/* Extracts this engine's 14-bit fixed-point (yaw, pitch, roll) -- the same
 * 0..0x3FFF tsin/tcos table-index convention as getangle()/getworldangles()
 * (control.c/transfrm.c) -- from a local-to-world rotation matrix in the
 * same [row][col] convention as vk1..vk9/vr_camera_compose()'s out_rot.
 * Mirrors getworldangles()'s own azimuth-then-elevation-then-bank
 * decomposition (transfrm.c:908-993), using the matrix's forward column
 * (col 2) and roll vector = minus column 0 (the engine's tilt rotates
 * columns 0 and 1 about the forward axis, and the roll vector equals minus
 * column 0). Intermediate "rotate by azimuth/elevation" steps use plain
 * trigonometry (not the tsin/tcos tables), since their outputs only feed the
 * next angle-extraction step, which re-quantizes to a 14-bit index anyway. */
void vr_camera_angles_from_rotation(const float rot[3][3],
                                     int *out_yaw, int *out_pitch, int *out_roll);

/* --- VR eye projection view-basis extraction ----------------------------
 * Used by vr_runtime.c's build_mvp_vr_eye_projection() to turn an ALREADY
 * fully eye-composed camera into the rotation/position its asymmetric-FOV
 * projection needs. base_rot/base_pos are in the SAME [row][col]/
 * vk1..vk9+viewx,y,z convention as vr_camera_compose()'s base_rot/out_rot
 * above -- by the time build_mvp_vr_eye_projection() runs, Task 2's
 * vr_camera_compose_apply() has already composed this eye's live head pose
 * into those globals, and base_rot/base_pos here are just that frame's
 * snapshot of them (read from base_proj->view/base_cam->viewX,Y,Z).
 *
 * out_combined_rot = S * transpose(base_rot) (S = diag(1,1,-1)): first
 * transpose to go from the game's local-to-world rotation to its
 * world-to-camera rotation (R_true, the same "view[col][row]" relationship
 * build_mvp() itself relies on, scene_render_gpu.c:831), then apply S to
 * convert out of the game's own +Z-forward convention into OpenXR/GL's
 * -Z-forward eye-space convention that the kept asymmetric projection
 * expects. out_eye_pos is base_pos COPIED THROUGH UNCHANGED -- no offset
 * of any kind is added.
 *
 * Deliberately takes NO pose/quaternion/XrView input of any kind: there is
 * nothing here for a caller to feed a live eye pose into, so nothing
 * calling this function can re-apply a head pose a second time. That is
 * the whole point -- composing the live per-eye head pose is
 * vr_camera_compose()'s job alone, done once, upstream of this function;
 * see tests/vr_camera_compose_test.c's case (c2) for the regression check
 * that calls this function directly against a known already-composed
 * camera and confirms out_eye_pos comes back exactly unchanged. */
void vr_camera_view_basis(const float base_rot[3][3], const float base_pos[3],
                           float out_combined_rot[3][3], float out_eye_pos[3]);

/* --- Active-eye-pose state, owned by this module ------------------------
 * Set by vr_present_stereo_frame() (vr_runtime.c) immediately before its own
 * calculateview() pre-call for this eye, and cleared immediately after
 * draw_road() returns for that eye. Scoped this tightly so no other
 * calculateview() call site in the game (menus, show_3dmap()'s track-map
 * preview, non-VR play, the editor) is ever affected -- vr_camera_compose_
 * apply() returns false (and touches nothing) whenever this hasn't been
 * called, which is every single non-VR calculateview() call and every VR
 * call outside this exact bracket. */
void vr_camera_compose_set_active_eye(float qx, float qy, float qz, float qw,
                                       float px, float py, float pz,
                                       float meters_to_world);
void vr_camera_compose_clear_active_eye(void);

/* Called from calculateview()'s single post-step (view.c), after
 * calculatetransform() has already written vk1..vk9/viewx/viewy/viewz for
 * this call. rot/pos on entry hold the just-computed, not-yet-composed
 * camera (read naively from vk1..vk9/viewx,y,z). If an eye pose is active,
 * composes it into rot/pos in place (modifying them), writes the matching
 * 14-bit angles to out_yaw/out_pitch/out_roll, and returns true -- the
 * caller must then write vk1..vk9/viewx/y/z/worlddirn/worldelev/worldtilt
 * from the results. Returns false (and writes NOTHING to any output) if no
 * eye pose is active. */
bool vr_camera_compose_apply(float rot[3][3], float pos[3],
                              int *out_yaw, int *out_pitch, int *out_roll);

#ifdef __cplusplus
}
#endif
#endif /* VR_CAMERA_COMPOSE_H */
