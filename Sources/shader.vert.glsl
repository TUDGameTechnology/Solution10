attribute vec3 pos;
attribute vec2 tex;
attribute vec3 nor;
varying vec2 texCoord;
varying vec3 normal;
uniform mat4 P;
uniform mat4 V;
uniform mat4 M;

void kore() {
	gl_Position = P * V * M * vec4(pos.x, pos.y, pos.z, 1.0);
	texCoord = tex;
	normal = (V * M * vec4(nor, 0.0)).xyz;
}
