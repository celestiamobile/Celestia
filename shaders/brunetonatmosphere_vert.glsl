// SPDX-License-Identifier: BSD-3-Clause
// Derived from Eric Bruneton's precomputed atmospheric scattering demo.

uniform mat4 model_from_view;
uniform mat4 view_from_clip;

layout(location = 0) in vec4 in_Position;

out vec3 view_ray;

void main()
{
    view_ray =
        (model_from_view * vec4((view_from_clip * in_Position).xyz, 0.0)).xyz;
    gl_Position = in_Position;
}
