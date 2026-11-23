/*
 * Direct3D 9 fixed-function draws.
 *
 * A draw without a vertex shader or without a pixel shader uses the
 * fixed-function pipeline of Direct3D 9. This file makes vs_3_0 and ps_3_0
 * token streams that do the same work as the fixed-function state of the
 * draw. The shader translator (rt_d3d9_shader.c) then compiles them like
 * the shaders of the program.
 *
 * Vertex processing (no vertex shader):
 *   POSITION is transformed by WORLD * VIEW * PROJECTION. POSITIONT
 *   (D3DFVF_XYZRHW) is in screen pixels and gets the inverse viewport
 *   transform. COLOR0 and COLOR1 go to the pixel stage; without them the
 *   diffuse color is (1, 1, 1, 1) and the specular color is (0, 0, 0, 0).
 *   TEXCOORD0..7 go to the pixel stage without change.
 *   Constants: c0..c3 the columns of the matrix, c4 and c5 the viewport
 *   scale and offset.
 * Pixel processing (no pixel shader): the texture stages
 *   (D3DTSS_COLOROP, D3DTSS_ALPHAOP and their arguments, D3DTSS_RESULTARG,
 *   D3DTSS_TEXCOORDINDEX), then D3DRS_SPECULARENABLE. Constants: c0
 *   D3DRS_TEXTUREFACTOR, c1..c8 D3DTSS_CONSTANT of stages 0..7.
 *
 * Fixed-function state that this file does not implement stops the
 * process with a message: lighting, vertex blending, user clip planes, fog,
 * texture transforms, generated texture coordinates, D3DTOP_PREMODULATE and
 * the bump mapping operations.
 *
 * The draw keeps the shader constants of the program: d3d_ff_begin saves
 * the registers that it writes, d3d_ff_end puts them back.
 */
#include "rt_d3d9_int.h"

/* ---------------------------------------------------------------- tokens */

enum { T_TEMP = 0, T_INPUT = 1, T_CONST = 2, T_OUTPUT = 6, T_COLOROUT = 8, T_SAMPLER = 10 };
enum {
    O_MOV = 1, O_ADD = 2, O_MAD = 4, O_MUL = 5, O_RCP = 6, O_DP3 = 8, O_LRP = 18, O_M4x4 = 20, O_DCL = 31,
    O_TEX = 66, O_DEF = 81
};

#define SWZ_XYZW 0xE4u
#define SWZ_WWWW 0xFFu
#define M_XYZ 0x7u
#define M_W 0x8u
#define M_ALL 0xFu
#define MAX_TOKENS 1024u

typedef struct tokbuf {
    uint32_t t[MAX_TOKENS];
    uint32_t n;
} tokbuf;

static void put(tokbuf *b, uint32_t v)
{
    if (b->n >= MAX_TOKENS) rt_fatal("D3D9: fixed-function shader of more than %u tokens", MAX_TOKENS);
    b->t[b->n++] = v;
}

static uint32_t reg(uint32_t type, uint32_t num)
{
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | num;
}

static uint32_t dst(uint32_t type, uint32_t num, uint32_t mask, bool sat)
{
    return reg(type, num) | (mask << 16) | (sat ? (1u << 20) : 0u);
}

static uint32_t src(uint32_t type, uint32_t num, uint32_t swz, bool neg)
{
    return reg(type, num) | (swz << 16) | (neg ? (1u << 24) : 0u);
}

static void ins(tokbuf *b, uint32_t op, uint32_t n, const uint32_t *p)
{
    uint32_t i;
    put(b, op | (n << 24));
    for (i = 0; i < n; i++) put(b, p[i]);
}

static void ins2(tokbuf *b, uint32_t op, uint32_t d, uint32_t s0)
{
    uint32_t p[2] = { d, s0 };
    ins(b, op, 2, p);
}

static void ins3(tokbuf *b, uint32_t op, uint32_t d, uint32_t s0, uint32_t s1)
{
    uint32_t p[3] = { d, s0, s1 };
    ins(b, op, 3, p);
}

static void ins4(tokbuf *b, uint32_t op, uint32_t d, uint32_t s0, uint32_t s1, uint32_t s2)
{
    uint32_t p[4] = { d, s0, s1, s2 };
    ins(b, op, 4, p);
}

/* dcl of an input or output register with a usage. */
static void dcl(tokbuf *b, uint32_t usage, uint32_t index, uint32_t type, uint32_t num)
{
    uint32_t p[2] = { 0x80000000u | usage | (index << 16), dst(type, num, M_ALL, false) };
    ins(b, O_DCL, 2, p);
}

static void dcl_sampler(tokbuf *b, uint32_t stt, uint32_t num)
{
    uint32_t p[2] = { 0x80000000u | (stt << 27), dst(T_SAMPLER, num, M_ALL, false) };
    ins(b, O_DCL, 2, p);
}

static void def(tokbuf *b, uint32_t num, float v)
{
    uint32_t p[5] = { dst(T_CONST, num, M_ALL, false), 0, 0, 0, 0 };
    uint32_t i;
    for (i = 1; i < 5; i++) memcpy(&p[i], &v, 4);
    ins(b, O_DEF, 5, p);
}

/* The shader object for a token stream (cached; the cache keeps one
   reference). */
static rt_com_object *shader_for(D3DDevice *d, bool ps, const tokbuf *b)
{
    NSData *key = [NSData dataWithBytes:b->t length:b->n * 4u];
    NSValue *v;
    rt_com_object *o;
    if (d->ff_shaders == nil) d->ff_shaders = [NSMutableDictionary new];
    v = d->ff_shaders[key];
    if (v != nil) return (rt_com_object *)v.pointerValue;
    if (d->ff_shaders.count >= 256u) d3d_ff_release(d);
    o = d3d_shader_from_tokens(d, ps, b->t, b->n);
    d->ff_shaders[key] = [NSValue valueWithPointer:o];
    rt_log("D3D9: fixed-function draw: new %s shader of %u tokens (%lu generated shaders)", ps ? "pixel" : "vertex",
           b->n, (unsigned long)d->ff_shaders.count);
    return o;
}

void d3d_ff_release(D3DDevice *d)
{
    NSArray<NSValue *> *all = d->ff_shaders.allValues;
    [d->ff_shaders removeAllObjects];
    for (NSValue *v in all) rt_com_release((rt_com_object *)v.pointerValue);
}

/* ---------------------------------------------------------------- vertex stage */

static rt_com_object *ff_vertex_shader(D3DDevice *d, bool *positiont)
{
    D3DDecl *dc = d3d_decl(d->decl);
    tokbuf b = { .n = 0 };
    int pos = -1, col[2] = { -1, -1 }, tc[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    uint32_t e, nv = 0, j;
    *positiont = false;
    for (e = 0; e < dc->n; e++)
    {
        uint32_t u = dc->el[e].usage, i = dc->el[e].index;
        if ((u == D3DDECLUSAGE_POSITION || u == D3DDECLUSAGE_POSITIONT) && i == 0 && pos < 0)
        {
            pos = (int)e;
            *positiont = u == D3DDECLUSAGE_POSITIONT;
        }
        else if (u == D3DDECLUSAGE_COLOR && i < 2 && col[i] < 0)
            col[i] = (int)e;
        else if (u == D3DDECLUSAGE_TEXCOORD && i < 8 && tc[i] < 0)
            tc[i] = (int)e;
    }
    if (pos < 0) rt_fatal("D3D9: a fixed-function draw has no POSITION or POSITIONT in its vertex declaration");
    put(&b, 0xFFFE0300u); /* vs_3_0 */
    def(&b, 6, 1.0f);
    def(&b, 7, 0.0f);
    /* Inputs: v0 position, then the colors and texture coordinates. */
    dcl(&b, dc->el[pos].usage, 0, T_INPUT, nv++);
    {
        uint32_t vc[2] = { 0, 0 }, vt[8] = { 0 };
        for (j = 0; j < 2; j++)
            if (col[j] >= 0)
            {
                vc[j] = nv;
                dcl(&b, D3DDECLUSAGE_COLOR, j, T_INPUT, nv++);
            }
        for (j = 0; j < 8; j++)
            if (tc[j] >= 0)
            {
                vt[j] = nv;
                dcl(&b, D3DDECLUSAGE_TEXCOORD, j, T_INPUT, nv++);
            }
        /* Outputs: o0 position, o1 diffuse, o2 specular, o3.. texture
           coordinates. */
        dcl(&b, D3DDECLUSAGE_POSITION, 0, T_OUTPUT, 0);
        dcl(&b, D3DDECLUSAGE_COLOR, 0, T_OUTPUT, 1);
        dcl(&b, D3DDECLUSAGE_COLOR, 1, T_OUTPUT, 2);
        {
            uint32_t no = 3;
            for (j = 0; j < 8; j++)
                if (tc[j] >= 0) dcl(&b, D3DDECLUSAGE_TEXCOORD, j, T_OUTPUT, no++);
        }
        if (*positiont)
        {
            /* r0.xyz = v0.xyz * c4 + c5 (normalized device coordinates),
               w = 1 / rhw, clip = ndc * w. */
            ins4(&b, O_MAD, dst(T_TEMP, 0, M_XYZ, false), src(T_INPUT, 0, SWZ_XYZW, false),
                 src(T_CONST, 4, SWZ_XYZW, false), src(T_CONST, 5, SWZ_XYZW, false));
            ins2(&b, O_RCP, dst(T_TEMP, 0, M_W, false), src(T_INPUT, 0, SWZ_WWWW, false));
            ins3(&b, O_MUL, dst(T_OUTPUT, 0, M_XYZ, false), src(T_TEMP, 0, SWZ_XYZW, false),
                 src(T_TEMP, 0, SWZ_WWWW, false));
            ins2(&b, O_MOV, dst(T_OUTPUT, 0, M_W, false), src(T_TEMP, 0, SWZ_XYZW, false));
        }
        else
            ins3(&b, O_M4x4, dst(T_OUTPUT, 0, M_ALL, false), src(T_INPUT, 0, SWZ_XYZW, false),
                 src(T_CONST, 0, SWZ_XYZW, false));
        ins2(&b, O_MOV, dst(T_OUTPUT, 1, M_ALL, false),
             col[0] >= 0 ? src(T_INPUT, vc[0], SWZ_XYZW, false) : src(T_CONST, 6, SWZ_XYZW, false));
        ins2(&b, O_MOV, dst(T_OUTPUT, 2, M_ALL, false),
             col[1] >= 0 ? src(T_INPUT, vc[1], SWZ_XYZW, false) : src(T_CONST, 7, SWZ_XYZW, false));
        {
            uint32_t no = 3;
            for (j = 0; j < 8; j++)
                if (tc[j] >= 0)
                    ins2(&b, O_MOV, dst(T_OUTPUT, no++, M_ALL, false), src(T_INPUT, vt[j], SWZ_XYZW, false));
        }
    }
    put(&b, 0x0000FFFFu);
    return shader_for(d, false, &b);
}

/* ---------------------------------------------------------------- pixel stage */

/* D3DTOP */
enum {
    TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X, TOP_MODULATE4X, TOP_ADD,
    TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH, TOP_BLENDDIFFUSEALPHA, TOP_BLENDTEXTUREALPHA,
    TOP_BLENDFACTORALPHA, TOP_BLENDTEXTUREALPHAPM, TOP_BLENDCURRENTALPHA, TOP_PREMODULATE,
    TOP_MODULATEALPHA_ADDCOLOR, TOP_MODULATECOLOR_ADDALPHA, TOP_MODULATEINVALPHA_ADDCOLOR,
    TOP_MODULATEINVCOLOR_ADDALPHA, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE, TOP_DOTPRODUCT3, TOP_MULTIPLYADD,
    TOP_LERP
};

/* D3DTA */
enum { TA_DIFFUSE = 0, TA_CURRENT, TA_TEXTURE, TA_TFACTOR, TA_SPECULAR, TA_TEMP, TA_CONSTANT };
#define TA_COMPLEMENT 0x10u
#define TA_ALPHAREPLICATE 0x20u

/* Registers of the pixel shader: r0 current, r1 temp, r2 arg1, r3 arg2,
   r4 arg0, r5 the texture of the stage, r6 and r7 scratch; v0 diffuse,
   v1 specular, v2+n texture coordinate set n; c9 1, c10 0.5, c11 0,
   c12 2, c13 4. */
enum { R_CUR = 0, R_TEMP, R_A1, R_A2, R_A0, R_TEX, R_S0, R_S1 };
enum { C_ONE = 9, C_HALF = 10, C_ZERO = 11, C_TWO = 12, C_FOUR = 13 };

static const char *const op_names[] = { "", "DISABLE", "SELECTARG1", "SELECTARG2", "MODULATE", "MODULATE2X",
                                        "MODULATE4X", "ADD", "ADDSIGNED", "ADDSIGNED2X", "SUBTRACT", "ADDSMOOTH",
                                        "BLENDDIFFUSEALPHA", "BLENDTEXTUREALPHA", "BLENDFACTORALPHA",
                                        "BLENDTEXTUREALPHAPM", "BLENDCURRENTALPHA", "PREMODULATE",
                                        "MODULATEALPHA_ADDCOLOR", "MODULATECOLOR_ADDALPHA",
                                        "MODULATEINVALPHA_ADDCOLOR", "MODULATEINVCOLOR_ADDALPHA", "BUMPENVMAP",
                                        "BUMPENVMAPLUMINANCE", "DOTPRODUCT3", "MULTIPLYADD", "LERP" };

/* The arguments that an operation reads: bit 0 arg0, bit 1 arg1, bit 2
   arg2. */
static uint32_t op_args(uint32_t op)
{
    switch (op)
    {
    case TOP_SELECTARG1: return 2u;
    case TOP_SELECTARG2: return 4u;
    case TOP_MULTIPLYADD:
    case TOP_LERP: return 7u;
    default: return 6u;
    }
}

static void check_op(uint32_t stage, uint32_t op, bool alpha)
{
    if (op < TOP_DISABLE || op > TOP_LERP)
        rt_fatal("D3D9: texture stage %u: %s operation %u is not a D3DTEXTUREOP", stage, alpha ? "alpha" : "color", op);
    if (op == TOP_PREMODULATE || op == TOP_BUMPENVMAP || op == TOP_BUMPENVMAPLUMINANCE ||
        (alpha && (op == TOP_DOTPRODUCT3 || (op >= TOP_MODULATEALPHA_ADDCOLOR && op <= TOP_MODULATEINVCOLOR_ADDALPHA))))
        rt_fatal("D3D9: fixed-function texture stage %u: the %s operation D3DTOP_%s is not supported", stage,
                 alpha ? "alpha" : "color", op_names[op]);
}

static uint32_t arg_src(uint32_t stage, uint32_t a, uint32_t swz)
{
    switch (a & 0xFu)
    {
    case TA_DIFFUSE: return src(T_INPUT, 0, swz, false);
    case TA_CURRENT: return src(T_TEMP, R_CUR, swz, false);
    case TA_TEXTURE: return src(T_TEMP, R_TEX, swz, false);
    case TA_TFACTOR: return src(T_CONST, 0, swz, false);
    case TA_SPECULAR: return src(T_INPUT, 1, swz, false);
    case TA_TEMP: return src(T_TEMP, R_TEMP, swz, false);
    case TA_CONSTANT: return src(T_CONST, 1u + stage, swz, false);
    default: rt_fatal("D3D9: texture stage %u: argument 0x%x is not a D3DTA value", stage, a);
    }
}

/* Load argument a into register r (mask m). */
static void load_arg(tokbuf *b, uint32_t stage, uint32_t a, uint32_t r, uint32_t m)
{
    uint32_t swz = (a & TA_ALPHAREPLICATE) ? SWZ_WWWW : SWZ_XYZW;
    uint32_t s = arg_src(stage, a, swz);
    if (a & ~(0xFu | TA_COMPLEMENT | TA_ALPHAREPLICATE))
        rt_fatal("D3D9: texture stage %u: argument 0x%x has unknown modifiers", stage, a);
    if (a & TA_COMPLEMENT)
        ins3(b, O_ADD, dst(T_TEMP, r, m, false), src(T_CONST, C_ONE, SWZ_XYZW, false), s | (1u << 24));
    else
        ins2(b, O_MOV, dst(T_TEMP, r, m, false), s);
}

static uint32_t t_(uint32_t r, uint32_t swz) { return src(T_TEMP, r, swz, false); }
static uint32_t c_(uint32_t c, bool neg) { return src(T_CONST, c, SWZ_XYZW, neg); }

/* Write operation op of the arguments in r2, r3, r4 to register dr (mask m). */
static void emit_op(tokbuf *b, uint32_t op, uint32_t dr, uint32_t m)
{
    uint32_t D = dst(T_TEMP, dr, m, true), S0 = dst(T_TEMP, R_S0, M_ALL, false), S1 = dst(T_TEMP, R_S1, M_ALL, false);
    uint32_t a1 = t_(R_A1, SWZ_XYZW), a2 = t_(R_A2, SWZ_XYZW), a0 = t_(R_A0, SWZ_XYZW), s0 = t_(R_S0, SWZ_XYZW);
    switch (op)
    {
    case TOP_SELECTARG1: ins2(b, O_MOV, D, a1); break;
    case TOP_SELECTARG2: ins2(b, O_MOV, D, a2); break;
    case TOP_MODULATE: ins3(b, O_MUL, D, a1, a2); break;
    case TOP_MODULATE2X:
        ins3(b, O_MUL, S0, a1, a2);
        ins3(b, O_MUL, D, s0, c_(C_TWO, false));
        break;
    case TOP_MODULATE4X:
        ins3(b, O_MUL, S0, a1, a2);
        ins3(b, O_MUL, D, s0, c_(C_FOUR, false));
        break;
    case TOP_ADD: ins3(b, O_ADD, D, a1, a2); break;
    case TOP_ADDSIGNED:
        ins3(b, O_ADD, S0, a1, a2);
        ins3(b, O_ADD, D, s0, c_(C_HALF, true));
        break;
    case TOP_ADDSIGNED2X:
        ins3(b, O_ADD, S0, a1, a2);
        ins3(b, O_ADD, S0, s0, c_(C_HALF, true));
        ins3(b, O_MUL, D, s0, c_(C_TWO, false));
        break;
    case TOP_SUBTRACT: ins3(b, O_ADD, D, a1, a2 | (1u << 24)); break;
    case TOP_ADDSMOOTH: /* a1 + a2 - a1 * a2 */
        ins4(b, O_MAD, S0, a1 | (1u << 24), a2, a1);
        ins3(b, O_ADD, D, s0, a2);
        break;
    case TOP_BLENDDIFFUSEALPHA: ins4(b, O_LRP, D, src(T_INPUT, 0, SWZ_WWWW, false), a1, a2); break;
    case TOP_BLENDTEXTUREALPHA: ins4(b, O_LRP, D, t_(R_TEX, SWZ_WWWW), a1, a2); break;
    case TOP_BLENDFACTORALPHA: ins4(b, O_LRP, D, src(T_CONST, 0, SWZ_WWWW, false), a1, a2); break;
    case TOP_BLENDCURRENTALPHA: ins4(b, O_LRP, D, t_(R_CUR, SWZ_WWWW), a1, a2); break;
    case TOP_BLENDTEXTUREALPHAPM: /* a1 + a2 * (1 - texture alpha) */
        ins3(b, O_ADD, S0, c_(C_ONE, false), t_(R_TEX, SWZ_WWWW) | (1u << 24));
        ins4(b, O_MAD, D, a2, s0, a1);
        break;
    case TOP_MODULATEALPHA_ADDCOLOR: ins4(b, O_MAD, D, t_(R_A1, SWZ_WWWW), a2, a1); break;
    case TOP_MODULATECOLOR_ADDALPHA: ins4(b, O_MAD, D, a1, a2, t_(R_A1, SWZ_WWWW)); break;
    case TOP_MODULATEINVALPHA_ADDCOLOR:
        ins3(b, O_ADD, S0, c_(C_ONE, false), t_(R_A1, SWZ_WWWW) | (1u << 24));
        ins4(b, O_MAD, D, s0, a2, a1);
        break;
    case TOP_MODULATEINVCOLOR_ADDALPHA:
        ins3(b, O_ADD, S0, c_(C_ONE, false), a1 | (1u << 24));
        ins4(b, O_MAD, D, s0, a2, t_(R_A1, SWZ_WWWW));
        break;
    case TOP_DOTPRODUCT3: /* saturate(dot(2 a1 - 1, 2 a2 - 1)) in all four components */
        ins4(b, O_MAD, S0, a1, c_(C_TWO, false), c_(C_ONE, true));
        ins4(b, O_MAD, S1, a2, c_(C_TWO, false), c_(C_ONE, true));
        ins3(b, O_DP3, dst(T_TEMP, dr, M_ALL, true), s0, t_(R_S1, SWZ_XYZW));
        break;
    case TOP_MULTIPLYADD: ins4(b, O_MAD, D, a1, a2, a0); break;
    case TOP_LERP: ins4(b, O_LRP, D, a0, a1, a2); break;
    default: rt_fatal("D3D9: fixed-function operation %u is not supported", op);
    }
}

static bool uses_texture(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t m = op_args(op);
    if (op == TOP_BLENDTEXTUREALPHA || op == TOP_BLENDTEXTUREALPHAPM) return true;
    return ((m & 1u) && (a0 & 0xFu) == TA_TEXTURE) || ((m & 2u) && (a1 & 0xFu) == TA_TEXTURE) ||
           ((m & 4u) && (a2 & 0xFu) == TA_TEXTURE);
}

static uint32_t stage_texture_type(D3DDevice *d, uint32_t stage)
{
    D3DTexture *t;
    if (d->tex[stage] == NULL) return D3DSTT_2D;
    t = d3d_tex(d->tex[stage]);
    return t->cube ? D3DSTT_CUBE : t->volume ? D3DSTT_VOLUME : D3DSTT_2D;
}

static rt_com_object *ff_pixel_shader(D3DDevice *d)
{
    tokbuf b = { .n = 0 };
    uint32_t i, nst = 0, tex_mask = 0, tc_mask = 0;
    /* The stages up to the first disabled color operation. */
    for (i = 0; i < 8; i++)
    {
        uint32_t *s = d->tss[i], cop = s[1], aop = s[4];
        if (cop == TOP_DISABLE) break;
        check_op(i, cop, false);
        if (aop != TOP_DISABLE) check_op(i, aop, true);
        if (s[11] >> 16)
            rt_fatal("D3D9: fixed-function texture stage %u: generated texture coordinates (D3DTSS_TEXCOORDINDEX "
                     "0x%x) are not supported", i, s[11]);
        if (s[24] != 0)
            rt_fatal("D3D9: fixed-function texture stage %u: texture transforms (D3DTSS_TEXTURETRANSFORMFLAGS 0x%x) "
                     "are not supported", i, s[24]);
        if (s[28] != TA_CURRENT && s[28] != TA_TEMP)
            rt_fatal("D3D9: texture stage %u: D3DTSS_RESULTARG 0x%x is not CURRENT or TEMP", i, s[28]);
        if (uses_texture(cop, s[26], s[2], s[3]) || (aop != TOP_DISABLE && uses_texture(aop, s[27], s[5], s[6])))
        {
            tex_mask |= 1u << i;
            tc_mask |= 1u << (s[11] & 7u);
            if ((s[11] & 0xFFFFu) > 7u)
                rt_fatal("D3D9: texture stage %u: D3DTSS_TEXCOORDINDEX %u", i, s[11] & 0xFFFFu);
        }
        nst++;
    }
    put(&b, 0xFFFF0300u); /* ps_3_0 */
    def(&b, C_ONE, 1.0f);
    def(&b, C_HALF, 0.5f);
    def(&b, C_ZERO, 0.0f);
    def(&b, C_TWO, 2.0f);
    def(&b, C_FOUR, 4.0f);
    dcl(&b, D3DDECLUSAGE_COLOR, 0, T_INPUT, 0);
    dcl(&b, D3DDECLUSAGE_COLOR, 1, T_INPUT, 1);
    for (i = 0; i < 8; i++)
        if (tc_mask & (1u << i)) dcl(&b, D3DDECLUSAGE_TEXCOORD, i, T_INPUT, 2u + i);
    for (i = 0; i < nst; i++)
        if (tex_mask & (1u << i)) dcl_sampler(&b, stage_texture_type(d, i), i);
    ins2(&b, O_MOV, dst(T_TEMP, R_CUR, M_ALL, false), src(T_INPUT, 0, SWZ_XYZW, false));
    ins2(&b, O_MOV, dst(T_TEMP, R_TEMP, M_ALL, false), c_(C_ZERO, false));
    for (i = 0; i < nst; i++)
    {
        uint32_t *s = d->tss[i], cop = s[1], aop = s[4], m, out = s[28] == TA_TEMP ? R_TEMP : R_CUR;
        if (tex_mask & (1u << i))
            ins3(&b, O_TEX, dst(T_TEMP, R_TEX, M_ALL, false), src(T_INPUT, 2u + (s[11] & 7u), SWZ_XYZW, false),
                 src(T_SAMPLER, i, SWZ_XYZW, false));
        /* Color: the arguments with all four components (some operations
           read the alpha of arg1). */
        m = op_args(cop);
        if (m & 1u) load_arg(&b, i, s[26], R_A0, M_ALL);
        if (m & 2u) load_arg(&b, i, s[2], R_A1, M_ALL);
        if (m & 4u) load_arg(&b, i, s[3], R_A2, M_ALL);
        emit_op(&b, cop, out, M_XYZ);
        if (cop == TOP_DOTPRODUCT3 || aop == TOP_DISABLE) continue; /* the alpha of the result stays */
        m = op_args(aop);
        if (m & 1u) load_arg(&b, i, s[27], R_A0, M_W);
        if (m & 2u) load_arg(&b, i, s[5], R_A1, M_W);
        if (m & 4u) load_arg(&b, i, s[6], R_A2, M_W);
        emit_op(&b, aop, out, M_W);
    }
    if (d->rs[RS_SPECULARENABLE])
        ins3(&b, O_ADD, dst(T_TEMP, R_CUR, M_XYZ, true), t_(R_CUR, SWZ_XYZW), src(T_INPUT, 1, SWZ_XYZW, false));
    ins2(&b, O_MOV, dst(T_COLOROUT, 0, M_ALL, false), t_(R_CUR, SWZ_XYZW));
    put(&b, 0x0000FFFFu);
    return shader_for(d, true, &b);
}

/* ---------------------------------------------------------------- draw */

static void color4(uint32_t c, float *v)
{
    v[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
    v[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
    v[2] = (float)(c & 0xFFu) / 255.0f;
    v[3] = (float)(c >> 24) / 255.0f;
}

void d3d_ff_begin(D3DDevice *d, d3d_ff_saved *sv)
{
    sv->vs = d->vs;
    sv->ps = d->ps;
    sv->ff_vs = d->vs == NULL;
    sv->ff_ps = d->ps == NULL;
    if (d->rs[RS_FOGENABLE]) rt_fatal("D3D9: fixed-function fog (D3DRS_FOGENABLE) is not supported");
    if (sv->ff_vs)
    {
        bool positiont;
        if (d->rs[RS_LIGHTING]) rt_fatal("D3D9: fixed-function lighting (D3DRS_LIGHTING) is not supported");
        if (d->rs[151]) rt_fatal("D3D9: fixed-function vertex blending (D3DRS_VERTEXBLEND %u) is not supported", d->rs[151]);
        if (d->rs[152]) rt_fatal("D3D9: fixed-function user clip planes (D3DRS_CLIPPLANEENABLE 0x%x) are not supported", d->rs[152]);
        d->vs = ff_vertex_shader(d, &positiont);
        memcpy(sv->vs_c, d->vs_c, sizeof sv->vs_c);
        if (positiont)
        {
            /* Inverse of the viewport transform of Direct3D 9. */
            float x = (float)d->viewport[0], y = (float)d->viewport[1], w = (float)d->viewport[2],
                  h = (float)d->viewport[3], zn = d3d_f(d->viewport[4]), zf = d3d_f(d->viewport[5]);
            float dz = zf - zn;
            if (w <= 0.0f || h <= 0.0f) rt_fatal("D3D9: fixed-function draw with an empty viewport");
            d->vs_c[4][0] = 2.0f / w;
            d->vs_c[4][1] = -2.0f / h;
            d->vs_c[4][2] = dz != 0.0f ? 1.0f / dz : 0.0f;
            d->vs_c[4][3] = 0.0f;
            d->vs_c[5][0] = -1.0f - 2.0f * x / w;
            d->vs_c[5][1] = 1.0f + 2.0f * y / h;
            d->vs_c[5][2] = dz != 0.0f ? -zn / dz : 0.0f;
            d->vs_c[5][3] = 0.0f;
        }
        else
        {
            /* Row vectors: v * WORLD * VIEW * PROJECTION. c_j is column j. */
            float wv[16], wvp[16];
            const float *w = d->transforms[256], *v = d->transforms[2], *p = d->transforms[3];
            int r, c, k;
            for (r = 0; r < 4; r++)
                for (c = 0; c < 4; c++)
                {
                    float s = 0.0f;
                    for (k = 0; k < 4; k++) s += w[r * 4 + k] * v[k * 4 + c];
                    wv[r * 4 + c] = s;
                }
            for (r = 0; r < 4; r++)
                for (c = 0; c < 4; c++)
                {
                    float s = 0.0f;
                    for (k = 0; k < 4; k++) s += wv[r * 4 + k] * p[k * 4 + c];
                    wvp[r * 4 + c] = s;
                }
            for (c = 0; c < 4; c++)
                for (r = 0; r < 4; r++) d->vs_c[c][r] = wvp[r * 4 + c];
        }
        d->vs_const_ver++;
    }
    if (sv->ff_ps)
    {
        uint32_t i;
        d->ps = ff_pixel_shader(d);
        memcpy(sv->ps_c, d->ps_c, sizeof sv->ps_c);
        color4(d->rs[RS_TEXTUREFACTOR], d->ps_c[0]);
        for (i = 0; i < 8; i++) color4(d->tss[i][32], d->ps_c[1 + i]);
        d->ps_const_ver++;
    }
}

void d3d_ff_end(D3DDevice *d, const d3d_ff_saved *sv)
{
    if (sv->ff_vs)
    {
        memcpy(d->vs_c, sv->vs_c, sizeof sv->vs_c);
        d->vs_const_ver++;
    }
    if (sv->ff_ps)
    {
        memcpy(d->ps_c, sv->ps_c, sizeof sv->ps_c);
        d->ps_const_ver++;
    }
    d->vs = sv->vs;
    d->ps = sv->ps;
}
