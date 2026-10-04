#ifndef VR_CAMERA_COMPOSE_H
#define VR_CAMERA_COMPOSE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Low-level 3x3 matrix helpers --------------------------------------
 * Naive [row][col] array reads throughout (m[r][c]), matching
 * vr_runtime.c's existing quat_to_mat3()/mat3_transpose()/mat3_flip_row2()
 * convention exactly. This header holds the single shared copy of this math,
 * so vr_runtime.c uses it rather than keeping its own duplicates. */
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
 * SAME relationship the projection path's combined_rot uses, but this
 * function composes the live head pose itself
 * (the projection path derives combined_rot = Hᵗ*S*Vᵗ = (V*S*H)ᵗ);
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
 * above -- by the time build_mvp_vr_eye_projection() runs,
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

/* The game camera of the eye being drawn as calculateview() computed it,
 * BEFORE the head pose was composed in: its world position and its 14-bit
 * yaw (vr_camera_angles_from_rotation() of the uncomposed rotation, the flat
 * game's worlddirn). The same for both eyes and for every head pose. For
 * world objects the flat game turns with the camera heading (worlddirn),
 * which in a VR eye is the head's yaw. Returns false, writing nothing,
 * outside the active-eye bracket or before vr_camera_compose_apply() ran in
 * it (always in the flat game). */
bool vr_camera_compose_base_view(float out_pos[3], int *out_yaw);

/* --- VR eye sky/ground half-plane ---------------------------------------
 * The flat game splits its sky and ground clear colours along a horizon line
 * computed from VIEWDIST and the flat window (game_render_draw_sky()). A VR
 * eye has its own, wider, asymmetric FOV, so that line sits at the wrong
 * height in the eye (about 2.4x too far from the centre vertically) and
 * moves faster than the world as the head pitches or rolls: whole regions
 * flip between sky and ground colour and distant buildings look like they
 * float above, or sink into, the ground.
 *
 * This computes the eye's true horizon instead. base_rot is the eye's fully
 * composed camera rotation in the vk1..vk9 [row][col] convention (the same
 * input vr_camera_view_basis() takes); tan_* are the tangents of the eye's
 * XrFovf angles (left/down negative). World +Z is up. On return, an eye NDC
 * point (x, y) (y up, before the shader compiler's Y flip) looks at sky when
 * a*x + b*y + c > 0 and at ground when it is < 0: the view ray through that
 * point, (X, Y, -1) in OpenXR eye space with X = lerp(tan_left, tan_right)
 * and Y = lerp(tan_down, tan_up), has a positive world-up component. */
void vr_eye_sky_halfplane(const float base_rot[3][3],
                          float tan_left, float tan_right,
                          float tan_up, float tan_down,
                          float *out_a, float *out_b, float *out_c);

/* --- VR eye projection of a camera-space point --------------------------
 * Screen-space effects (smoke, sparks, car name tags) are projected on the
 * CPU from engine camera space -- vx = d.(vk1,vk4,vk7) (right), vy =
 * d.(vk2,vk5,vk8) (up), vz = d.(vk3,vk6,vk9) (forward depth), d = world
 * point minus viewx/viewy/viewz -- with the flat window's VIEWDIST, xbase,
 * ybase and scr_size. A VR eye has its own, wider, asymmetric FOV, so for a
 * 42-degree half-FOV eye that puts them 1.8x (VIEWDIST 200) to 2.45x
 * (VIEWDIST 270) too far from the centre vertically, 1.1x to 1.5x
 * horizontally, shifted up by the flat window's ybase offset, and makes
 * them slide against the world as the head turns.
 *
 * This maps the same camera-space point through the eye's own projection
 * instead, the one build_mvp_vr_eye_projection() (vr_runtime.c) gives the
 * world geometry: vr_camera_view_basis() makes eye X = vx, Y = vy, Z = -vz,
 * then the OpenXR asymmetric projection from the XrFovf tangents (left/down
 * negative) with near/far planes in world units and depth in [0, 1]. On
 * return out_x/out_y are eye NDC (y up, before the shader compiler's Y flip,
 * the convention the screen-quad path uses) and out_z the NDC depth (not
 * clamped). Returns false, writing nothing, if vz <= 0. */
bool vr_eye_view_to_ndc(float vx, float vy, float vz,
                        float tan_left, float tan_right,
                        float tan_up, float tan_down,
                        float near_plane, float far_plane,
                        float *out_x, float *out_y, float *out_z);

/* --- VR scenery billboard yaw ------------------------------------------
 * Trees and advert balloons (building plans 9, 10 and 15) are flat cards in
 * the plan's Y-Z plane. The flat game turns them to the camera's heading,
 * yaw = worlddirn, so the card's normal, plan +X turned by the yaw, is
 * (cos yaw, sin yaw) in world X-Y: the camera's horizontal forward
 * (calculatetransform(): vk3 = cos dir cos elev, vk6 = sin dir cos elev).
 * In a VR eye worlddirn is the head's yaw, so every billboard spun in place
 * as the head turned and stayed face-on to wherever the player looked.
 *
 * This gives the yaw, in degrees (the unit building.c's plan transform
 * takes), that turns the card to face a viewer at (view_x, view_y) instead:
 * the direction from the viewer to the object, atan2(dy, dx), the same
 * angle convention as getangle(). It equals the flat game's yaw when the
 * camera looks straight at the object, and does not depend on where the
 * head points. fallback_deg is returned when the viewer is straight above or
 * below the object (no horizontal direction). */
float vr_billboard_yaw_deg(float obj_x, float obj_y, float view_x, float view_y,
                           float fallback_deg);

#ifdef __cplusplus
}
#endif
#endif /* VR_CAMERA_COMPOSE_H */
