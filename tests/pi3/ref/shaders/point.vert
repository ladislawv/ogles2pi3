attribute vec3 pos;
void main() { gl_Position = vec4(pos, 1.0); gl_PointSize = 8.0; }
