#version 150

uniform sampler2D u_tex;

in vec2 v_uv;

out vec4 FragColor;

void main()
{
  FragColor = texture(u_tex, v_uv);
}
