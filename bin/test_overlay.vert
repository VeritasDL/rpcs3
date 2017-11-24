#version 420
out vec2 tc0;

void main()
{
	vec2 positions[] = {vec2(-1., -1.), vec2(1., -1.), vec2(-1., 1.), vec2(1., 1.)};
	vec2 coords[] = {vec2(0., 0.), vec2(1., 0.), vec2(0., 1.), vec2(1., 1.)};
	gl_Position = vec4(positions[gl_VertexID % 4], 0., 1.);
	tc0 = coords[gl_VertexID % 4];
}
