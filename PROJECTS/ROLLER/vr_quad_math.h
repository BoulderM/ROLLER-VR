/* PROJECTS/ROLLER/vr_quad_math.h
 *
 * Pure math for anchoring quad layers (no OpenXR/SDL/Vulkan types), so it can
 * be unit-tested on its own (tests/vr_menu_quad_yaw_test.c). OpenXR axis
 * convention throughout: +X right, +Y up, -Z forward; quaternions (x,y,z,w). */
#ifndef VR_QUAD_MATH_H
#define VR_QUAD_MATH_H

/* Heading of an orientation, flattened to the horizontal plane: the angle (in
 * radians, about +Y, counter-clockwise seen from above, 0 = facing -Z) of the
 * orientation's forward vector projected onto the X-Z plane. Pitch and roll do
 * not change it. Undefined (but finite) when looking straight up or down. */
float vr_quad_yaw_from_quat(float qx, float qy, float qz, float qw);

/* A pose `distance` metres ahead of head_pos along heading `yaw` (at head
 * height), facing back towards the head: out_pos is head_pos + distance *
 * (-sin yaw, 0, -cos yaw), out_quat is the pure-yaw rotation (0, sin(yaw/2),
 * 0, cos(yaw/2)), whose -Z axis is that same heading, so a quad posed there
 * stands upright with its front face towards the viewer. */
void vr_quad_yaw_anchor(const float head_pos[3], float yaw, float distance,
                        float out_pos[3], float out_quat[4]);

#endif /* VR_QUAD_MATH_H */
