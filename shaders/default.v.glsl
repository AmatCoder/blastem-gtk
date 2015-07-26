#version 110

attribute vec2 pos;
varying vec2 texcoord;
uniform float width;

void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	texcoord = sign(pos) * vec2(width/1024.0, 240.0/-512.0) + vec2(width/1024.0, 240.0/512.0);
}
