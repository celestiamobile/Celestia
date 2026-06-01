// SPDX-License-Identifier: BSD-3-Clause
// atmosphere_vert.glsl — companion vertex shader for atmosphere_frag.glsl.
// Drawn as a fullscreen quad: in_Position is the four ±1 corner positions
// of a triangle strip covering NDC [-1,1]^2. The renderer is responsible
// for binding an identity MVPMatrix so that set_vp produces the same
// z=1 (far plane) clip-space position regardless of camera; the fragment
// shader reconstructs an object-space view ray from v_ndc.

in vec4 in_Position;

out vec2 v_ndc;

void main()
{
    v_ndc = in_Position.xy;
    set_vp(vec4(in_Position.xy, 1.0, 1.0));
}
