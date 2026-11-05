#extension GL_EXT_draw_buffers : require
precision mediump float;
uniform vec4 color;
void main() { gl_FragData[0] = color; gl_FragData[1] = vec4(1.0) - color; }
