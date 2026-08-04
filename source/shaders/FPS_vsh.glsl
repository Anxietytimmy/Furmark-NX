#version 460

layout (location = 0) in vec2 inPos;
layout (location = 1) in vec3 inColor;

layout (location = 0) out vec3 v_color;

void main()
{
    v_color = inColor;
    gl_Position = vec4(inPos, 0.0, 1.0);
}