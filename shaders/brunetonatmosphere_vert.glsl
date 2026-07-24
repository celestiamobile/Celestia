// SPDX-License-Identifier: BSD-3-Clause
// Derived from Eric Bruneton's precomputed atmospheric scattering demo.

uniform mat4 model_from_view;
uniform mat4 view_from_clip;

layout(location = 0) in vec4 in_Position;

out vec3 view_ray;
out vec3 view_ray_view;

void main()
{
    view_ray_view = (view_from_clip * in_Position).xyz;
    view_ray =
        (model_from_view * vec4(view_ray_view, 0.0)).xyz;
    gl_Position = in_Position;
}
