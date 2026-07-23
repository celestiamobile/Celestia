// SPDX-FileCopyrightText: 2026 Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

layout(location = 0) in vec4 in_Position;

out vec3 brunetonPosition;

uniform int uScreenSpace;

void main(void)
{
    brunetonPosition = in_Position.xyz;
    if (uScreenSpace != 0)
        gl_Position = in_Position;
    else
        set_vp(in_Position);
}
