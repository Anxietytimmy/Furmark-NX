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

// NEON Dot product
inline float32x4_t dot(const vec3x4& a, const vec3x4& b){
    float32x4_t acc = vmulq_f32(a.x, b.x);
    acc = vfmaq_f32(acc, a.y, b.y);
    acc = vfmaq_f32(acc, a.z, b.z);
    return acc;
}

// Neon normalize
inline vec3x4 normalize(vec3x4 v){
    float32x4_t len2 = vfmaq_f32(vfmaq_f32(vmulq_f32(v.x, v.x), v.y, v.y), v.z, v.z);

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
// Reflections
// In my restless dreams, I see that town
// The mental hospital
inline vec3x4 reflect(const vec3x4& v, const vec3x4& n){
    float32x4_t two_d = vmulq_n_f32(dot(v, n), 2.0f);
    vec3x4 r;
    r.x = vfmsq_f32(v.x, n.x, two_d);  // vfmsq instead of vmlsq
    r.y = vfmsq_f32(v.y, n.y, two_d);
    r.z = vfmsq_f32(v.z, n.z, two_d);
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

// Cross (cam)
inline vec3f cross(const vec3f& a, const vec3f& b) {
    return vec3f(
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    );
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
        vfmaq_f32( vmulq_f32(v.z, v.z), v.y, v.y);

    len2 = vfmaq_f32(len2, v.x, v.x);

    float32x4_t invLen = fastRsqrt(len2);

    vec3x4 out;
    out.x = vmulq_f32(v.x, invLen);
    out.y = vmulq_f32(v.y, invLen);
    out.z = vmulq_f32(v.z, invLen);
    return out;
}


// NEW ARRIVALS IN INDIA 
// Maybe its those horse people I was talking about 
// Or thier cousins or something

// snap back to reality
// Or well I guess randomness

// ACTUALLY FUCKING VECTORIZED RNG THIS TIME GOOD GOD
// -------------------------------
// V E C R N G N E O N 
// -------------------------------

// 4-Lane XOR Shift (mainly so trace doesn't blow its head off)
inline uint32x4_t xorshift4(uint32x4_t& s) {
    s = veorq_u32(s, vshlq_n_u32(s, 13));
    s = veorq_u32(s, vshrq_n_u32(s, 17));
    s = veorq_u32(s, vshlq_n_u32(s,  5));
    return s;
}

// Convert uint32x4 into a float 1, to match scalar rng in trace
inline float32x4_t toFloat01(uint32x4_t x) {
    uint32x4_t m = vandq_u32(x, vdupq_n_u32(0xFFFFFF));
    return vmulq_n_f32(vcvtq_f32_u32(m), 1.0f / float(0xFFFFFF));
}

// Vec cosines/sines, were stuck on a different planet
// -----------------------------------------------
// F A S T S I N / C O S ( N E O N )
// -----------------------------------------------

// This technically has a .3% error rate, but I cannot be fucked to care

// We use horner's method of evaluating polynomials.
// Where a polynomial with degree N can be evaluated only using n mul/adds
// We could simply plug in n value to find at every polynomial variable, however this is much faster
// Take p(x) = 3x³ + 2x² - 5x + 1, we could simply plug in our desired X value, however instead
// We can nest the polynomial to reduce the number of mul needed, ie
// p(x) = 3x³ + 2x² - 5x + 1 --> (3x + 2)x² - 5x + 1 --> ((3x + 2)x + (-5))x + 1
// To get p(x) = (((3x + 2)x - 5)x + 1), reducing the required multiplications

// Taylor seires are considered, but we reduce their range and flip sign for full (0, 2pi)


// Sin4
inline float32x4_t sin4(float32x4_t phi) {
    // Pi constants
    const float32x4_t pi = vdupq_n_f32(3.14159265f);
    const float32x4_t pi_2 = vdupq_n_f32(1.57079633f);
    const float32x4_t two_pi = vdupq_n_f32(6.28318531f);

    // Reduce [0, 2pi] -> [0, pi], track signs
    uint32x4_t neg = vcgeq_f32(phi, pi);
    float32x4_t xr = vbslq_f32(neg, vsubq_f32(phi, pi), phi);

    // Reduce [0, pi] -> [0, pi/2]
    // Sin is symmetric @ pi/2
    uint32x4_t fold = vcgtq_f32(xr, pi_2);
    xr = vbslq_f32(fold, vsubq_f32(pi, xr), xr);
    
    // Remember the horner yap from earlier, yeah here it is, n = 7
    // in other words, fuck the moon
    // in other words, degree 7 horner on [0, pi/2]
    // le actual formulent
    // sin(x) = x*(1 - x^2 * (1/6 - x^2 * (1/120 - x^2 / 5040)))

    float32x4_t x2 = vmulq_f32(xr, xr);
    float32x4_t r = vfmsq_f32(vdupq_n_f32(1.0f/120.0f), x2, vdupq_n_f32(1.0f/5040.0f));
    r = vfmsq_f32(vdupq_n_f32(1.0f/6.0f), x2, r);
    r = vfmsq_f32(vdupq_n_f32(1.0f), x2, r);
    r = vmulq_f32(xr, r);

    // Flip sign for lanes that were [pi, 2pi]
    uint32x4_t signBit = vshlq_n_u32(neg, 31);
    return vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(r), signBit));    
}

//Cos4
inline float32x4_t cos4(float32x4_t phi) {
    // cos(phi) = sin(phi + pi/2), wrap it into [0, 2pi]
    float32x4_t shifted = vaddq_f32(phi, vdupq_n_f32(1.57079633f));
    float32x4_t two_pi = vdupq_n_f32(6.28318531f);
    uint32x4_t wrap = vcgeq_f32(shifted, two_pi);
    shifted = vbslq_f32(wrap, vsubq_f32(shifted, two_pi), shifted);
    return sin4(shifted);
}

// yall ready for linear algebra 
// Orthonormal bases, also known as, what the shit was Euler on man
// So, ONB are a set of vectors that are both orthogonal to each other and normalized such that L = 1
// Meaning that for any two vectors (u and v), the dot product is given by u * v = 0 if u does not equal v and u * u = 1
// Orthogonal in this case refers to two vectors which are perpendicular to each other

// -----------------------------------------------
// O N B
// -----------------------------------------------

// Uses branchless targets + bitangents from normals
// handles n.z = -1 with/o singulatiy
inline void buildONB4(const vec3x4& n, vec3x4& t, vec3x4& b){
    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t neg = vdupq_n_f32(-1.0f);   

    // sign = (n.z >= 0) ? 1 : -1
    uint32x4_t pos = vcgeq_f32(n.z, vdupq_n_f32(0.0f));
    float32x4_t sgn = vbslq_f32(pos, one, neg);

    // a = -1 / (sign + n.z) - set denominators as never zeros for unit normals
    float32x4_t denom = vaddq_f32(sgn, n.z);
    float32x4_t r = vrecpeq_f32(denom);
    r = vmulq_f32(vrecpsq_f32(denom, r), r);
    r = vmulq_f32(vrecpsq_f32(denom, r), r);
    float32x4_t a = vnegq_f32(r);

    // bcoef = nx * ny * a
    float32x4_t bc = vmulq_f32(vmulq_f32(n.x, n.y), a);

    // tan = (1 + sign*nx²*a,  sign*bcoef,  -sign*nx)
    t.x = vaddq_f32(one, vmulq_f32(sgn, vmulq_f32(vmulq_f32(n.x, n.y), a)));
    t.y = vmulq_f32(sgn, bc);
    t.z = vnegq_f32(vmulq_f32(sgn, n.x));

    // bitan = (bcoef,  sign + ny²*a,  -ny)
    b.x = bc;
    b.y = vaddq_f32(sgn, vmulq_f32(vmulq_f32(n.y, n.y), a));
    b.z = vnegq_f32(n.y);
}

// hell
// -----------------------------------------------
// C O S H E M I S P H E R E S A M P L E
// -----------------------------------------------

// Takes normal and rng states, returns cos weighted bounce directions
// no normalizations needed as we provide in unit lenght
inline vec3x4 cosineSampleHemisphere4(const vec3x4& n, uint32x4_t& rng){
    // 2 uniform floats per plane
    float32x4_t u1 = toFloat01(xorshift4(rng));
    float32x4_t u2 = toFloat01(xorshift4(rng));

    // r = sqrt(u1), phi = 2*pi*u2
    float32x4_t r = vsqrtq_f32(u1);
    float32x4_t phi = vmulq_n_f32(u2, 6.28318531f);

    // local disk coord + hemisphere projection
    float32x4_t lx = vmulq_f32(r, cos4(phi));
    float32x4_t ly = vmulq_f32(r, sin4(phi));
    float32x4_t lz = vsqrtq_f32(vmaxq_f32(vsubq_f32(vdupq_n_f32(1.0f), u1), vdupq_n_f32(0.0)));

    // build ONB from surface mat
    vec3x4 t, b;
    buildONB4(n, t, b);

    // use world space 
    vec3x4 d;
    d.x = vaddq_f32(vaddq_f32(vmulq_f32(lx, t.x), vmulq_f32(ly, b.x)), vmulq_f32(lz, n.x));
    d.y = vaddq_f32(vaddq_f32(vmulq_f32(lx, t.y), vmulq_f32(ly, b.y)), vmulq_f32(lz, n.y));
    d.z = vaddq_f32(vaddq_f32(vmulq_f32(lx, t.z), vmulq_f32(ly, b.z)), vmulq_f32(lz, n.z));
    return d;
}

// hello again
// its ya boy
// sleep paralyisis
// ---------------------------
// Black hole bullshit
// ---------------------------

// These are mainly for camera position computes, but they are useful outside of the black hole test.
// that Plus 4k lines is already good enough thank you

// dot producut, but ignores W
inline float neon_dot3(float32x4_t a, float32x4_t b)
{
    float32x4_t mul = vmulq_f32(a, b);
    float32x2_t lo = vget_low_f32(mul);
    float32x2_t hi = vget_high_f32(mul);
    float32x2_t sum = vpadd_f32(lo, lo);
    return vget_lane_f32(vadd_f32(sum, hi), 0);
}

// 3 component cross product
inline float32x4_t neon_cross3(float32x4_t a, float32x4_t b) {
    float32x4_t a_yzx = __builtin_shufflevector(a, a, 1, 2, 0, 3);
    float32x4_t a_zxy = __builtin_shufflevector(a, a, 2, 0, 1, 3);
    float32x4_t b_yzx = __builtin_shufflevector(b, b, 1, 2, 0 ,3);
    float32x4_t b_zxy = __builtin_shufflevector(b, b, 2, 0, 1, 3);
    return vsubq_f32(vmulq_f32(a_yzx, b_zxy), vmulq_f32(a_zxy, b_yzx));
}

// Normalize once
// Use newton's method for this
inline float32x4_t neon_normalize3(float32x4_t v)
{
    float32x2_t lenSq = vdup_n_f32(neon_dot3(v, v));
    float32x2_t est = vrsqrte_f32(lenSq);
    est = vmul_f32(est, vrsqrts_f32(vmul_f32(lenSq, est), est));
    return vmulq_f32(v, vcombine_f32(est, est));
}

// Store into float[3]
inline void neon_store3(float *p, float32x4_t v) 
{
    vst1q_lane_f32(p + 0, v, 0);
    vst1q_lane_f32(p + 1, v, 1);
    vst1q_lane_f32(p + 2, v, 2);
}

// GLSL permute
inline float32x4_t vpermute(float32x4_t x)
{
    float32x4_t v34 = vdupq_n_f32(34.0f);
    float32x4_t v1 = vdupq_n_f32(1.0f);
    float32x4_t v289 = vdupq_n_f32(289.0f);
    float32x4_t inv289 = vdupq_n_f32(1.0f / 289.0f);

    // ((x * 34.0) + 1.0) * x
    float32x4_t res = vmlaq_f32(v1, x, v34);
    res = vmulq_f32(res, x);

    float32x4_t quotient = vmulq_f32(res, inv289);

    float32x4_t floored = vrndmq_f32(quotient); 
    res = vmlsq_f32(res, floored, v289);

    return res;
}

// GLSL taylorInvSqrt
inline float32x4_t vtaylorInvSqrt(float32x4_t x)
{
    float32x4_t c1 = vdupq_n_f32(1.79284291400159f);
    float32x4_t c2 = vdupq_n_f32(0.85373472095314f);
    return vmlsq_f32(c1, x, c2);
}

// FastSinCos, because math.c is a meme
// Accurate to 6 decimals
static inline void fastSinCos(float angle, float* s, float* c)
{
    // Rudce ranges to [-pi, pi]
    const float INV_TWO_PI = 0.15915494309f;
    const float TWO_PI = 6.28318530718f;
    const float PI = 3.14159265359f;

    float x = angle - TWO_PI * floorf(angle * INV_TWO_PI + 0.5f);

    // Compute sin via +/- polynomia, only odd terms
    // coeffs from Remez algo over constants
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x3;

    *s = x + x3 * (-0.16666667163f) + x5 * ( 0.00833333842f) + x7 * (-0.00019840680f);

    // Comp cos via the same method, only even terms
    float x4 = x2 * x2;
    float x6 = x4 * x2;

    *c = 1.0f + x2 * (-0.49999997020f) + x4 * ( 0.04166664556f) + x6 * (-0.00138873165f);
}