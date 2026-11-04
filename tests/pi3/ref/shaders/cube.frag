precision mediump float;
uniform samplerCube tex;
varying vec3 d;
void main() { gl_FragColor = textureCube(tex, d); }
