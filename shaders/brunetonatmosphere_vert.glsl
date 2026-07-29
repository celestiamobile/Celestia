// SPDX-License-Identifier: BSD-3-Clause
// Derived from Eric Bruneton's precomputed atmospheric scattering demo.

uniform mat4 model_from_view;
uniform mat4 view_from_clip;
uniform mat4 clip_from_model;
uniform vec3 camera;
uniform int use_shell;
uniform float shell_radius;

layout(location = 0) in vec4 in_Position;

out vec3 view_ray;

void main()
{
    if (use_shell != 0)
    {
        // Scale the unit-sphere vertex to the shell radius.
        vec3 model_position = in_Position.xyz * shell_radius;
        view_ray = model_position - camera;
        gl_Position = clip_from_model * vec4(model_position, 1.0);
    }
    else
    {
        // in_Position is a full-screen quad vertex in clip space.
        vec3 view_ray_view = (view_from_clip * in_Position).xyz;
        view_ray =
            (model_from_view * vec4(view_ray_view, 0.0)).xyz;
        gl_Position = in_Position;
    }
}
