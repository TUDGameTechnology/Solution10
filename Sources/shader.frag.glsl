#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D tex;
varying vec2 texCoord;
varying vec3 normal;

void kore() {
	gl_FragColor = texture2D(tex, texCoord) + normal.x * 0.01; // replace with your BRDF
}
