#version 460

layout (location = 0) in vec2 v_uv;
layout (location = 0) out vec4 fragColor;

precision highp float;
precision highp int;

layout (std140, binding = 0) uniform Params
{
    vec2 u_resolution;
    float u_time;
    float _pad;
};

// Texture bindings
layout (binding = 0) uniform sampler2D u_texture1;
layout (binding = 1) uniform sampler2D u_texture2;
layout (binding = 2) uniform sampler2D u_texture3;

// Config
// March steps defines raymarch steps
// burn iters defines iterations for FMA functions
// aa taps is the aa samples
// tex per step is the texture grabs per each step
#define MARCH_STEPS 48
#define MARCH_FAR 128.0
#define BURN_ITERS 3
#define AA_TAPS 1
#define TEX_PER_STEP 1

#ifdef USE_MIPPED_SAMPLING
    #define SAMPLE(t, uv) texture(t, uv)
#else
    #define SAMPLE(t, uv) textureLod(t, uv, 0.0)
#endif

const float PI = 3.1416;
const float TAU = 2.0 * PI;

// I just want to see the world burn
vec4 burn(vec4 seed, float amt)
{
    // normalize seeb no matter if its fucking massive
    vec4 s = fract(seed * 0.015625) * 2.0 - 1.0;

    vec4 a = s;
    vec4 b = fract(s * 1.3247180 + 0.7071068) * 2.0 - 1.0;
    vec4 c = fract(s * 1.6180340 + 0.3183099) * 2.0 - 1.0;
    vec4 d = fract(s * 1.4142136 + 0.6180340) * 2.0 - 1.0;

    for(int i = 0; i < BURN_ITERS; ++i)
    {
        // Independent chains
        a = fma(a, vec4( 1.3247180), vec4( 0.7071068));
        b = fma(b, vec4(-1.2207440), vec4(-0.3183099));
        c = fma(c, vec4( 1.1892071), vec4( 0.6180340));
        d = fma(d, vec4(-1.1547005), vec4(-0.4142136));

        // cross feed
        a = fma(b, c, a);
        b = fma(c, d, b);
        c = fma(d, a, c);
        d = fma(a, b, d);

        // Bind without entropy colapse
        a = fma(fract(a), vec4(2.0), vec4(-1.0));
        b = fma(fract(b), vec4(2.0), vec4(-1.0));
        c = fma(fract(c), vec4(2.0), vec4(-1.0));
        d = fma(fract(d), vec4(2.0), vec4(-1.0));
    }
    // Make sure our output is always finite
    return (a + b + c + d) * amt;      
}


mat2 rot2D(float a)
{
    float sa = sin(a);
    float ca = cos(a);
    return mat2(ca, sa, -sa, ca);
}

// Comp rotation just at int
mat2 g_rotXY;
mat2 g_rotYZ;

void initRotation()
{
    g_rotXY = rot2D(sin(u_time * 0.8) * 0.25);
    g_rotYZ = rot2D(sin(u_time * 0.7) * 0.2);
}

void rotate(inout vec3 p)
{
    p.xy *= g_rotXY;
    p.yz *= g_rotYZ;
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

// Displacent
float displace(vec3 p, sampler2D tex)
{
    float s = 4.5;
    float u = s / TAU * atan(p.y, p.x);
    float z2 = p.z * p.z;
    float v = sign(p.z) / TAU * acos(clamp((z2 * sqrt(s * s + 1.0) + sqrt(max(1.0 - z2 * s * s, 0.0))) / (z2 + 1.0), -1.0, 1.0));
    vec2 uv = 2.0 * vec2(u, v);
    return SAMPLE(tex, 2.0 * vec2(u, v)).r * 0.06;
}


float rayMarch(vec3 ro, vec3 rd, out float sink)
{
    float dist = 0.0;
    sink = 0.0;

    for (int i = 0; i < MARCH_STEPS; i++) {
        vec3 p = ro + dist * rd;
        rotate(p);

        float hit = map(p);
        dist += hit;

        // displace
        dist -= displace(0.5 * p, u_texture2);

        // Clamp divergent rays
        dist = clamp(dist, -8.0, MARCH_FAR);

        // Fetch textures before burning
        float t = 0.0;
        for (int k = 0; k < TEX_PER_STEP; ++k)
        {
            vec2 st = p.xy * (1.0 + float(k) * 3.0) + vec2(float(i) * 0.061, u_time * 0.05);
            t += SAMPLE(u_texture3, st).r;
        }

        // Power core
        sink += dot(burn(vec4(p, dist) + t, 0.1), vec4(1.0));
        
        dist += sink * 1e-7;
    }
    return dist;
}

vec3 triPlanar(sampler2D tex, vec3 p, vec3 normal)
{
    normal = abs(normal);
    normal = normal * normal;           
    normal = normal * normal;
    normal = normal * normal;
    normal = normal * normal;           
    // n^16 close enough
    normal /= normal.x + normal.y + normal.z + 1e-6;
    p = p * 0.5 + 0.5;
    return (texture(tex, p.xy) * normal.z +
            texture(tex, p.xz) * normal.y +
            texture(tex, p.yz) * normal.x).rgb;
}

vec3 render(vec2 offset)
{
    vec2 uv = (2.0 * (gl_FragCoord.xy + offset) - u_resolution.xy) / u_resolution.y;

    vec3 ro = vec3(0, 0, -1.0);
    vec3 rd = normalize(vec3(uv, 1.0));

    float sink;
    float dist = rayMarch(ro, rd, sink);

    vec3 col;
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

        float h = fma(sin(8.0 * phi), 0.5, 0.5);

        vec2 st;
        st.x = 3.0 * phi / PI;
        st.y = u_time * 0.5 + PI / (rho + 0.1 * smoothstep(0.45, 0.5, h));

        col += SAMPLE(u_texture3, st).rgb;

        float occ = smoothstep(0.0, 0.45, h) - smoothstep(0.5, 1.0, h);
        col *= 1.0 - 0.45 * occ * rho;
        col *= rho;
    }
    return col + sink * 1e-8;
}

void main()
{
    initRotation();

    vec3 color = render(vec2( 0.125,  0.375))
               + render(vec2(-0.125, -0.375))
               + render(vec2(-0.375,  0.125))
               + render(vec2( 0.375, -0.125));

    fragColor = vec4(color * 0.25, 1.0);
}
