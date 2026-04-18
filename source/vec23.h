// vec2/3 replacements for scalar/NEON functions

// Vec2 replacement, because I like spaget
struct vec2f {
    float x, y;

    inline vec2f() {}
    inline vec2f(float x_, float y_) : x(x_), y(y_) {}
};


// I FUCKING LOVE XENON BULBS NEON HELL YEAHHHH
// In full non shitpost fashion, NEON needs 128bit SIMD, or 4x  F32 lanes
// V3F below works for scalars, but for this we need more functinos
// -------------------------------
// V E C 3 X 4 N E O N
// -------------------------------
struct vec3x4 {
    float32x4_t x, y, z;
};

struct Hit4 {
    float32x4_t t;
    vec3x4 normal;
    uint32x4_t material;
};


// NEON Dot product
inline float32x4_t dot(const vec3x4& a, const vec3x4& b){
    return vmlaq_f32(vmlaq_f32(vmulq_f32(a.x, b.x), a.y, b.y), a.z, b.z);
}

// Neon normalize
inline vec3x4 normalize(vec3x4 v){
    float32x4_t len2 = vmlaq_f32(vmlaq_f32(vmulq_f32(v.x, v.x), v.y, v.y), v.z, v.z);

    float32x4_t invLen = vrsqrteq_f32(len2);

    // NR refinement
    // Physx, quackclulus, quarks and stuff
    // basically, as my undervolted sleep deprived brain matter understands it
    // this is such that it is a way to approximate the root of a function and then check using curvature information to refine the result
    invLen = vmulq_f32(vrsqrtsq_f32(vmulq_f32(len2, invLen), invLen), invLen);

    return {
        vmulq_f32(v.x, invLen),
        vmulq_f32(v.y, invLen),
        vmulq_f32(v.z, invLen)
    };
}


// NEON reflections
// holy fuck the hills are silent
inline vec3x4 reflect(vec3x4 v, vec3x4 n){
    float32x4_t d = dot(v, n);
    float32x4_t two = vdupq_n_f32(2.0f);

    vec3x4 r;
    r.x = vmlsq_f32(v.x, n.x, vmulq_f32(two, d));
    r.y = vmlsq_f32(v.y, n.y, vmulq_f32(two, d));
    r.z = vmlsq_f32(v.z, n.z, vmulq_f32(two, d));
    return r;
}


// Faster vec3 replacement
// -------------------------------
// V E C 3 F S C A L A R
// -------------------------------
struct vec3f {
    float x, y, z;

    inline vec3f () {}
    inline vec3f(float v) : x(v), y(v), z(v) {}
    inline vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// Vec3 operations, because A57 asked for adderal
inline vec3f operator+(const vec3f& a, const vec3f& b) {
    return vec3f(a.x + b.x, a.y + b.y, a.z + b.z);
}

inline vec3f operator-(const vec3f& a, const vec3f& b) {
    return vec3f(a.x - b.x, a.y - b.y, a.z - b.z);
}

inline vec3f operator-(const vec3f& v) {
    return vec3f(-v.x, -v.y, -v.z);
}

inline vec3f& operator*=(vec3f& a, const vec3f& b) {
    a.x *= b.x;
    a.y *= b.y;
    a.z *= b.z;
    return a;
}

inline vec3f& operator*=(vec3f& a, float b) {
    a.x *= b;
    a.y *= b;
    a.z *= b;
    return a;
}

inline vec3f operator*(const vec3f& a, float b) {
    return vec3f(a.x * b, a.y * b, a.z * b);
}

inline vec3f operator*(float b, const vec3f& a) {
    return a * b;
}

inline vec3f operator*(const vec3f& a, const vec3f& b) {
    return vec3f(a.x * b.x, a.y * b.y, a.z * b.z);
}

inline vec3f& operator+=(vec3f& a, const vec3f& b) {
    a.x += b.x; a.y += b.y; a.z += b.z;
    return a;
}

inline vec3f operator/(const vec3f& a, float b) {
    float inv = 1.0f / b;
    return vec3f(a.x * inv, a.y * inv, a.z * inv);
}

inline vec3f& operator/=(vec3f& a, float b) {
    float inv = 1.0f / b;
    a.x *= inv;
    a.y *= inv;
    a.z *= inv;
    return a;
}

// Dot product
inline float dot(const vec3f& a, const vec3f& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

// Normalization
// Replace with NEON rsqrt once we know this doesn't explode
inline vec3f normalize(const vec3f& v) {
    float len2 = dot(v, v);
    float invLen = 1.0f / sqrtf(len2 + 1e-20f);
    return v * invLen;
}

// Reflections
// In my restless dreams, I see that town
// The mental hospital
inline vec3f reflect(const vec3f& v, const vec3f& n) {
    return v - n * (2.0f * dot(v, n));
}

// clamp
inline vec3f clamp(const vec3f& v, float minv, float maxv) {
    return vec3f(
        fminf(fmaxf(v.x, minv), maxv),
        fminf(fmaxf(v.y, minv), maxv),
        fminf(fmaxf(v.z, minv), maxv)
    );
}

// Cross (cam)
inline vec3f cross(const vec3f& a, const vec3f& b) {
    return vec3f(
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    );
}


// Well this is cursed
// Scalar for neon functions
inline vec3f neon_normalize(const vec3f& v)
{
    float len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    float inv = 1.0f / sqrtf(len2 + 1e-20f);
    return { v.x * inv, v.y * inv, v.z * inv };
}

inline vec3f neon_reflect(const vec3f& v, const vec3f& n)
{
    float d = v.x*n.x + v.y*n.y + v.z*n.z;
    return {
        v.x - 2.0f * d * n.x,
        v.y - 2.0f * d * n.y,
        v.z - 2.0f * d * n.z
    };
}

// More NEON helper functions
inline float32x4_t selectf(uint32x4_t m, float32x4_t a, float32x4_t b) {
    return vbslq_f32(m, a, b);
}

inline uint32x4_t selecti(uint32x4_t m, uint32x4_t a, uint32x4_t b) {
    return vbslq_u32(m, a, b);
}

inline uint32x4_t andMask(uint32x4_t a, uint32x4_t b) {
    return vandq_u32(a, b);
}

// Pack/unpacking functions
inline vec3x4 pack4(const vec3f* v)
{
    vec3x4 out;

    out.x = vdupq_n_f32(0.0f);
    out.y = vdupq_n_f32(0.0f);
    out.z = vdupq_n_f32(0.0f);

    out.x = vsetq_lane_f32(v[0].x, out.x, 0);
    out.x = vsetq_lane_f32(v[1].x, out.x, 1);
    out.x = vsetq_lane_f32(v[2].x, out.x, 2);
    out.x = vsetq_lane_f32(v[3].x, out.x, 3);

    out.y = vsetq_lane_f32(v[0].y, out.y, 0);
    out.y = vsetq_lane_f32(v[1].y, out.y, 1);
    out.y = vsetq_lane_f32(v[2].y, out.y, 2);
    out.y = vsetq_lane_f32(v[3].y, out.y, 3);

    out.z = vsetq_lane_f32(v[0].z, out.z, 0);
    out.z = vsetq_lane_f32(v[1].z, out.z, 1);
    out.z = vsetq_lane_f32(v[2].z, out.z, 2);
    out.z = vsetq_lane_f32(v[3].z, out.z, 3);

    return out;
}

inline void unpack4(const vec3x4& v, vec3f* out)
{
    float xx[4], yy[4], zz[4];

    vst1q_f32(xx, v.x);
    vst1q_f32(yy, v.y);
    vst1q_f32(zz, v.z);

    for(int i = 0; i < 4; i++)
    {
        out[i].x = xx[i];
        out[i].y = yy[i];
        out[i].z = zz[i];
    }
}

// fast reciprocals
static inline float32x4_t fastRecip(float32x4_t x)
{
    float32x4_t r = vrecpeq_f32(x);
    r = vmulq_f32(vrecpsq_f32(x, r), r);
    r = vmulq_f32(vrecpsq_f32(x, r), r);
    return r;
}

// fast sqrt
static inline float32x4_t fastRsqrt(float32x4_t x)
{
    float32x4_t r = vrsqrteq_f32(x);
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(x, r), r));
    r = vmulq_f32(r, vrsqrtsq_f32(vmulq_f32(x, r), r));
    return r;
}

// blursed
static inline vec3x4 normalizeFast(const vec3x4& v)
{
    float32x4_t len2 =
        vmlaq_f32(
            vmulq_f32(v.z, v.z),
            v.y,
            v.y);

    len2 = vmlaq_f32(len2, v.x, v.x);

    float32x4_t invLen = fastRsqrt(len2);

    vec3x4 out;
    out.x = vmulq_f32(v.x, invLen);
    out.y = vmulq_f32(v.y, invLen);
    out.z = vmulq_f32(v.z, invLen);
    return out;
}