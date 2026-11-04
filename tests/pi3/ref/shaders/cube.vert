attribute vec3 pos;
attribute vec3 dir;
varying vec3 d;
void main() { gl_Position = vec4(pos, 1.0); d = dir; }
