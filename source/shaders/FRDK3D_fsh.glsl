#version 460

layout (location = 0) in vec2 v_uv;
layout (location = 0) out vec4 fragColor;

layout (binding = 0) uniform Params
{
    vec2 u_resolution;
    float u_time;
    // 16B align
    float _pad;
};

// Texture bindings
layout (binding = 0) uniform sampler2D u_texture1;
layout (binding = 1) uniform sampler2D u_texture2;
layout (binding = 2) uniform sampler2D u_texture3;

const float PI = 3.1416;
const float TAU = 2.0 * PI;

float acosFast(float x)
{
    float negate = float(x < 0.0);
    x = abs(x);
    float ret = -0.0187293;
    ret = ret * x + 0.0742610;
    ret = ret * x - 0.2121144;
    ret = ret * x + 1.5707288;
    ret = ret * sqrt(1.0 - x);
    return ret - 2.0 * negate * ret + negate * 3.14159265358979;
}

float displace(vec3 p, sampler2D tex)
{
    const float s = 4.5;
    const float sq_s2p1 = 4.609772228646444;
    float u = s / TAU * atan(p.y / p.x);

    float x = clamp((p.z * p.z * sq_s2p1 + sqrt(1.0 - p.z * p.z * s * s)) / (p.z * p.z + 1.0), -1.0, 1.0);
    float v = sign(p.z) / TAU * acosFast(x);

    vec2 uv = 2.0 * vec2(u, v);
    float disp = texture(tex, uv).r;
    return disp * 0.06;
}

mat2 rot2D(float a)
{
    float sa = sin(a);
    float ca = cos(a);
    return mat2(ca, sa, -sa, ca);
}

void rotate(inout vec3 p)
{
    p.xy *= rot2D(sin(u_time * 0.8) * 0.25);
    p.yz *= rot2D(sin(u_time * 0.7) * 0.2);
}

float map(vec3 p)
{
    float dist = length(vec2(length(p.xy) - 0.6, p.z)) - 0.22;
    return dist * 0.7;
}

vec3 getNormal(vec3 p)
{
    vec2 e = vec2(0.01, 0.0);
    vec3 n = vec3(map(p)) - vec3(map(p - e.xyy), map(p - e.yxy), map(p - e.yyx));
    return normalize(n);
    
}

float rayMarch(vec3 ro, vec3 rd)
{
    float dist = 0.0;
    float final_dist = 101.0;
    float is_active = 1.0;
    for (int i = 0; i < 48; i++)
    {
        vec3 p = ro + dist * rd;
        rotate(p);
        float hit = map(p);

        float step_dist = hit;
        step_dist -= displace(0.5 * p, u_texture2);

        vec3 q = p;
        q = q * 1.37 + 0.13;
        q = q * q - 0.17;
        q = q * 0.91 + q.yzx * 0.09;
        step_dist += dot(q, q) * 1e-5;

        vec4 r0 = vec4(p, dist);
        vec4 r1 = sin(r0 * 3.1);
        vec4 r2 = cos(r1 * 2.7);
        vec4 r3 = r2 * r1;
        vec4 r4 = normalize(r3);
        step_dist += dot(r4, vec4(1e-5));

        float hit_surface = step(abs(hit), 0.0001);
        float hit_far = step(100.0, dist);

        float just_finished = clamp(hit_surface + hit_far, 0.0, 1.0) * is_active;

        final_dist = mix(final_dist, dist, just_finished);

        // HOLY FUCK WE ARE CLOSE NOW
        dist += mix(2.0, step_dist, is_active);

    }
    return dist;
}

vec3 triPlanar(sampler2D tex, vec3 p, vec3 normal)
{
    normal = abs(normal);
    normal = pow(normal, vec3(15));
    normal /= normal.x + normal.y + normal.z;
    p = p * 0.5 + 0.5;
    return (texture(tex, p.xy) * normal.z + texture(tex, p.xz) * normal.y + texture(tex, p.yz) * normal.x).rgb;
}

vec3 render(vec2 offset)
{
    vec2 uv = (2.0 * (gl_FragCoord.xy + offset) - u_resolution.xy) / u_resolution.y;
    vec3 col = vec3(0);

    vec3 ro = vec3(0, 0, -1.0);
    vec3 rd = normalize(vec3(uv, 1.0));

    float dist = rayMarch(ro, rd);

    if (dist < 100.0)
    {
        vec3 p = ro + dist * rd;
        rotate(p);
        col += triPlanar(u_texture1, p * 1.0, getNormal(p));
    }
    else
    {
        float phi = atan(uv.y, uv.x);
        float rho = length(uv) + 0.2;
        phi += sin(0.3 * rho - 0.5 * u_time);
        float h = sin(8.0 * phi) * 0.5 + 0.5;

        vec2 st;
        st.x = 3.0 * phi / PI;
        st.y = u_time * 0.5 + PI / (rho + 0.1 * smoothstep(0.45, 0.5, h));

        col += texture(u_texture3, st).rgb;

        float occ = smoothstep(0.0, 0.45, h) - smoothstep(0.5, 1.0, h);
        col *= 1.0 - 0.45 * occ * rho;
        col *= rho;
    }
    return col;
}

// This is technically named wrong but I am too fucking lazy to change it
vec3 renderAAx8()
{
    vec4 e = vec4(0.125, -0.125, 0.375, -0.375);
    vec3 colAA = render(e.xy) + render(e.yw) + render(e.wx) + render(e.zy);
    return colAA /= 4;
}

void main()
{
    vec3 color = renderAAx8();
    fragColor = vec4(color, 1.0);
}
