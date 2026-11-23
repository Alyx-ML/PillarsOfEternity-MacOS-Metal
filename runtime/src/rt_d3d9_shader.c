/*
 * Direct3D 9 shader bytecode to Metal Shading Language (see
 * rt_d3d9_shader.h).
 *
 * Token format (D3D9 DDK, d3d9types.h):
 *   version      0xFFFE#### (vertex) or 0xFFFF#### (pixel), major.minor
 *   instruction  bits 0-15 opcode, 16-23 opcode control, 24-27 length in
 *                dwords (shader model 2 and later), 28 predicated
 *   parameter    bits 0-10 register number, 28-30 and 11-12 register
 *                type, 13 relative addressing (an address token follows);
 *                destination: 16-19 write mask, 20-23 result modifier;
 *                source: 16-23 swizzle, 24-27 source modifier
 *   comment      0xFFFE with the length in bits 16-30
 *   end          0x0000FFFF
 *
 * Shader model 1: only ps_1_1, ps_1_2 and ps_1_3. The Unity player makes
 * ps_1_1 shaders for the texture combiners of fixed-function shaders
 * ("D3D9 Combiners" in PillarsOfEternity.exe). Differences from shader
 * model 2:
 *   - The instruction token has no length; sm1_params gives it.
 *   - Bit 30 of the instruction token is co-issue ("+"): the instruction
 *     and the instruction before it read their sources before either
 *     writes.
 *   - There are no declarations. v0 and v1 are COLOR0 and COLOR1; t# is
 *     texture register #. "tex t#" samples the texture of stage # at
 *     TEXCOORD#; the bound texture gives the texture type and
 *     D3DTSS_TEXTURETRANSFORMFLAGS gives the projection (the sm1_tex
 *     variant of d3d_shader_msl_ps).
 *   - r0 is the color output.
 *   - Constants are clamped to -1..1.
 *   - Source modifiers _bias, _bx2, 1-x and _x2; destination shifts _x2,
 *     _x4, _x8, _d2, _d4 and _d8.
 * The texture address instructions (texbem, texm3x*, texreg2*, texdp3*)
 * are not supported; their translation fails.
 */
#include "rt_d3d9_shader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 65536u
#define MAX_SOURCE (4u << 20)
#define MAX_LABELS 64
#define MAX_CALL_DEPTH 4

/* Opcodes. */
enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT,
    OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2,
    OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM,
    OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB,
    OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80, OP_DEF = 81,
    OP_CMP = 88, OP_DP2ADD = 90,
    OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96,
    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};

/* Register types. */
enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, /* RT_TEXTURE in pixel shaders */
    RT_RASTOUT = 4, RT_ATTROUT = 5, RT_OUTPUT = 6,        /* RT_TEXCRDOUT before vs_3_0 */
    RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONST2 = 11, RT_CONST3 = 12,
    RT_CONST4 = 13, RT_CONSTBOOL = 14, RT_LOOP = 15, RT_TEMPFLOAT16 = 16, RT_MISCTYPE = 17, RT_LABEL = 18,
    RT_PREDICATE = 19
};

#define REL_BIT (1u << 13)
#define DST_SAT (1u << 20)
#define DST_CENTROID (4u << 20)

static uint32_t rtype(uint32_t p) { return ((p >> 28) & 7u) | ((p >> 8) & 0x18u); }
static uint32_t rnum(uint32_t p) { return p & 0x7FFu; }
static uint32_t wmask(uint32_t p) { return (p >> 16) & 0xFu; }

static const char *usage_name(uint32_t u)
{
    static const char *const n[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                                     "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT",
                                     "COLOR", "FOG", "DEPTH", "SAMPLE" };
    return u < sizeof n / sizeof n[0] ? n[u] : NULL;
}

void d3dsh_text_free(d3dsh_text *t)
{
    free(t->s);
    memset(t, 0, sizeof *t);
}

void d3d_shader_free(d3d_shader *sh)
{
    free(sh->tokens);
    memset(sh, 0, sizeof *sh);
}

/* ---------------------------------------------------------------- instruction walk */

typedef struct inst {
    uint32_t op, ctl, pos, n; /* pos: index of the instruction token; n: parameter dwords */
    bool pred, coissue;
    const uint32_t *p;
} inst;

/* Parameter count of a ps_1_1 to ps_1_3 instruction (destination and
   sources; D3D9 DDK). Returns -1 for an opcode that shader model 1 does not
   have. */
static int sm1_params(uint32_t op)
{
    switch (op)
    {
    case OP_NOP:
    case OP_PHASE: return 0;
    case OP_TEXCOORD:
    case OP_TEXKILL:
    case OP_TEX:
    case 87: /* texdepth */ return 1;
    case OP_MOV:
    case OP_RCP:
    case OP_RSQ:
    case OP_EXP:
    case OP_LOG:
    case OP_LIT:
    case OP_FRC:
    case OP_EXPP:
    case OP_LOGP:
    case 67: /* texbem */
    case 68: /* texbeml */
    case 69: /* texreg2ar */
    case 70: /* texreg2gb */
    case 71: /* texm3x2pad */
    case 72: /* texm3x2tex */
    case 73: /* texm3x3pad */
    case 74: /* texm3x3tex */
    case 75: /* texm3x3diff */
    case 77: /* texm3x3vspec */
    case 82: /* texreg2rgb */
    case 83: /* texdp3tex */
    case 84: /* texm3x2depth */
    case 85: /* texdp3 */
    case 86: /* texm3x3 */ return 2;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_DP3:
    case OP_DP4:
    case OP_MIN:
    case OP_MAX:
    case OP_SLT:
    case OP_SGE:
    case OP_DST:
    case OP_M4x4:
    case OP_M4x3:
    case OP_M3x4:
    case OP_M3x3:
    case OP_M3x2:
    case 76: /* texm3x3spec */
    case 89: /* bem */ return 3;
    case OP_MAD:
    case OP_LRP:
    case OP_CND:
    case OP_CMP: return 4;
    case OP_DEF: return 5;
    default: return -1;
    }
}

/* Read the instruction at *pos. Returns false at the end token or on a
   format error (err set). Comments are skipped. tk[0] is the version
   token. */
static bool next_inst(const uint32_t *tk, uint32_t ntk, uint32_t *pos, inst *in, bool *end, char *err, size_t errn)
{
    bool sm1 = ((tk[0] >> 8) & 0xFFu) < 2u;
    *end = false;
    for (;;)
    {
        uint32_t t, op;
        if (*pos >= ntk)
        {
            snprintf(err, errn, "shader has no end token");
            return false;
        }
        t = tk[*pos];
        op = t & 0xFFFFu;
        if (op == OP_END)
        {
            *end = true;
            return false;
        }
        if (op == OP_COMMENT)
        {
            uint32_t len = (t >> 16) & 0x7FFFu;
            if (len > ntk - *pos - 1u)
            {
                snprintf(err, errn, "comment extends past the end of the shader");
                return false;
            }
            *pos += 1u + len;
            continue;
        }
        in->op = op;
        in->ctl = (t >> 16) & 0xFFu;
        if (sm1)
        {
            int np = sm1_params(op);
            if (np < 0)
            {
                snprintf(err, errn, "opcode %u at token %u is not a shader model 1 instruction", op, *pos);
                return false;
            }
            in->pred = false;
            in->coissue = (t & (1u << 30)) != 0;
            in->n = (uint32_t)np;
        }
        else
        {
            in->pred = (t & (1u << 28)) != 0;
            in->coissue = false;
            in->n = (t >> 24) & 0xFu;
        }
        in->pos = *pos;
        if (in->n > ntk - *pos - 1u)
        {
            snprintf(err, errn, "instruction at token %u extends past the end of the shader", *pos);
            return false;
        }
        in->p = tk + *pos + 1u;
        *pos += 1u + in->n;
        return true;
    }
}

/* ---------------------------------------------------------------- parse */

static d3dsh_io *add_io(d3dsh_io *arr, int *n, uint32_t usage, uint32_t index, uint32_t reg, uint32_t mask,
                        bool centroid, char *err, size_t errn)
{
    d3dsh_io *e;
    if (*n >= D3DSH_MAX_IO)
    {
        snprintf(err, errn, "more than %d input or output declarations", D3DSH_MAX_IO);
        return NULL;
    }
    e = &arr[(*n)++];
    e->usage = (uint8_t)usage;
    e->index = (uint8_t)index;
    e->reg = (uint8_t)reg;
    e->mask = (uint8_t)mask;
    e->centroid = centroid;
    return e;
}

static bool find_io(const d3dsh_io *arr, int n, uint32_t usage, uint32_t index)
{
    int i;
    for (i = 0; i < n; i++)
        if (arr[i].usage == usage && arr[i].index == index) return true;
    return false;
}

bool d3d_shader_parse(d3d_shader *sh, const uint32_t *tokens, uint32_t max_tokens, char *err, size_t errn)
{
    uint32_t pos = 1, ntk, major, minor;
    inst in;
    bool end = false;
    memset(sh, 0, sizeof *sh);
    if (max_tokens < 2u)
    {
        snprintf(err, errn, "shader is too short");
        return false;
    }
    if (max_tokens > MAX_TOKENS) max_tokens = MAX_TOKENS;
    if ((tokens[0] >> 16) == 0xFFFEu)
        sh->is_ps = false;
    else if ((tokens[0] >> 16) == 0xFFFFu)
        sh->is_ps = true;
    else
    {
        snprintf(err, errn, "bad version token 0x%08x", tokens[0]);
        return false;
    }
    major = (tokens[0] >> 8) & 0xFFu;
    minor = tokens[0] & 0xFFu;
    sh->version = major << 8 | minor;
    if (sh->is_ps && major == 1 ? (minor < 1u || minor > 3u) : (major < 2 || major > 3))
    {
        snprintf(err, errn, "%s_%u_%u is not supported (only ps_1_1 to ps_1_3 and shader models 2 and 3)",
                 sh->is_ps ? "ps" : "vs", major, minor);
        return false;
    }
    /* Find the end token, so that the copy has the exact length. */
    ntk = max_tokens;
    while (next_inst(tokens, ntk, &pos, &in, &end, err, errn))
    {
    }
    if (!end) return false;
    ntk = pos + 1u;
    sh->tokens = malloc(sizeof(uint32_t) * ntk);
    if (sh->tokens == NULL)
    {
        snprintf(err, errn, "out of memory");
        return false;
    }
    memcpy(sh->tokens, tokens, sizeof(uint32_t) * ntk);
    sh->ntokens = ntk;

    pos = 1;
    while (next_inst(sh->tokens, ntk, &pos, &in, &end, err, errn))
    {
        if (sh->version < 0x200 && (in.op == OP_TEX || in.op == OP_TEXCOORD || in.op == OP_TEXKILL))
        {
            /* The parameter is texture register t# (# < 4). */
            if (rtype(in.p[0]) != RT_ADDR || rnum(in.p[0]) >= 4u)
            {
                snprintf(err, errn, "instruction %u at token %u does not use t0 to t3", in.op, in.pos);
                goto fail;
            }
            if (in.op == OP_TEX) sh->sampler_type[rnum(in.p[0])] = D3DSTT_2D; /* the draw gives the type */
        }
        if (in.op != OP_DCL && in.op != OP_DEF && in.op != OP_DEFI && in.op != OP_DEFB)
        {
            /* Float constant reads. Parameter 0 is the destination, except
               for the instructions that have only sources. A source with
               REL_BIT is followed by its address token. */
            uint32_t limit = sh->is_ps ? D3DSH_PS_CONST_F : D3DSH_VS_CONST_F, i;
            bool no_dst = in.op == OP_TEXKILL || in.op == OP_IF || in.op == OP_IFC || in.op == OP_REP ||
                          in.op == OP_LOOP || in.op == OP_BREAKC || in.op == OP_BREAKP || in.op == OP_CALL ||
                          in.op == OP_CALLNZ || in.op == OP_LABEL;
            for (i = no_dst ? 0u : 1u; i < in.n; i++)
            {
                uint32_t tok = in.p[i];
                if (rtype(tok) == RT_CONST)
                {
                    if (tok & REL_BIT)
                        sh->nconst_f = (uint16_t)limit;
                    else if (rnum(tok) < limit && rnum(tok) + 1u > sh->nconst_f)
                        sh->nconst_f = (uint16_t)(rnum(tok) + 1u);
                }
                if (tok & REL_BIT) i++;
            }
        }
        if (in.op == OP_DCL)
        {
            uint32_t u, r, t, reg;
            if (in.n < 2u) goto bad_len;
            u = in.p[0];
            r = in.p[1];
            t = rtype(r);
            reg = rnum(r);
            if (t == RT_SAMPLER)
            {
                uint32_t st = (u >> 27) & 0xFu;
                if (reg >= D3DSH_MAX_SAMPLERS || (st != D3DSTT_2D && st != D3DSTT_CUBE && st != D3DSTT_VOLUME))
                {
                    snprintf(err, errn, "bad sampler declaration s%u type %u", reg, st);
                    goto fail;
                }
                sh->sampler_type[reg] = (uint8_t)st;
            }
            else if (t == RT_INPUT || (sh->is_ps && t == RT_ADDR /* t# */))
            {
                uint32_t usage = u & 0x1Fu, index = (u >> 16) & 0xFu;
                if (sh->version < 0x300 && sh->is_ps)
                {
                    usage = t == RT_INPUT ? D3DDECLUSAGE_COLOR : D3DDECLUSAGE_TEXCOORD;
                    index = reg;
                }
                if (usage_name(usage) == NULL || reg >= D3DSH_MAX_IO)
                {
                    snprintf(err, errn, "bad input declaration (usage %u, register %u)", usage, reg);
                    goto fail;
                }
                if (sh->is_ps && sh->version < 0x300 && t == RT_ADDR) reg += 8u; /* t# after v# */
                if (add_io(sh->inputs, &sh->ninputs, usage, index, reg, wmask(r), (r & DST_CENTROID) != 0, err,
                           errn) == NULL)
                    goto fail;
            }
            else if (t == RT_OUTPUT && !sh->is_ps && sh->version >= 0x300)
            {
                uint32_t usage = u & 0x1Fu, index = (u >> 16) & 0xFu;
                if (usage_name(usage) == NULL || reg >= 12u)
                {
                    snprintf(err, errn, "bad output declaration (usage %u, register %u)", usage, reg);
                    goto fail;
                }
                if (find_io(sh->outputs, sh->noutputs, usage, index))
                {
                    snprintf(err, errn, "output %s%u is declared twice", usage_name(usage), index);
                    goto fail;
                }
                if (add_io(sh->outputs, &sh->noutputs, usage, index, reg, wmask(r), false, err, errn) == NULL)
                    goto fail;
            }
            else if (t == RT_MISCTYPE)
            {
                if (reg == 0) sh->uses_vpos = true;
                else if (reg == 1) sh->uses_vface = true;
                else
                {
                    snprintf(err, errn, "bad misc register %u", reg);
                    goto fail;
                }
            }
            else
            {
                snprintf(err, errn, "unsupported declaration of register type %u", t);
                goto fail;
            }
        }
        else if (in.op == OP_DEF || in.op == OP_DEFI)
        {
            d3dsh_def *d;
            uint32_t reg;
            if (in.n < 5u) goto bad_len;
            reg = rnum(in.p[0]);
            if (in.op == OP_DEF)
            {
                if (reg >= (sh->is_ps ? D3DSH_PS_CONST_F : D3DSH_VS_CONST_F) || sh->ndef_f >= D3DSH_MAX_DEFS)
                {
                    snprintf(err, errn, "bad def c%u", reg);
                    goto fail;
                }
                d = &sh->def_f[sh->ndef_f++];
            }
            else
            {
                if (reg >= D3DSH_CONST_I || sh->ndef_i >= D3DSH_CONST_I)
                {
                    snprintf(err, errn, "bad defi i%u", reg);
                    goto fail;
                }
                d = &sh->def_i[sh->ndef_i++];
            }
            d->reg = (uint16_t)reg;
            memcpy(d->v, in.p + 1, 16);
        }
        else if (in.op == OP_DEFB)
        {
            uint32_t reg;
            if (in.n < 2u) goto bad_len;
            reg = rnum(in.p[0]);
            if (reg >= D3DSH_CONST_B || sh->ndef_b >= D3DSH_CONST_B)
            {
                snprintf(err, errn, "bad defb b%u", reg);
                goto fail;
            }
            sh->def_b[sh->ndef_b].reg = (uint16_t)reg;
            sh->def_b[sh->ndef_b].v[0] = in.p[1];
            sh->ndef_b++;
        }
        else if (in.n > 0u && in.op != OP_TEXKILL && in.op != OP_IF && in.op != OP_IFC && in.op != OP_REP &&
                 in.op != OP_LOOP && in.op != OP_BREAKC && in.op != OP_BREAKP && in.op != OP_CALL &&
                 in.op != OP_CALLNZ && in.op != OP_LABEL)
        {
            uint32_t d = in.p[0], t = rtype(d), reg = rnum(d);
            if (t == RT_COLOROUT && reg < 4u) sh->color_outputs = (uint8_t)(sh->color_outputs | (1u << reg));
            if (t == RT_DEPTHOUT) sh->writes_depth = true;
            if (!sh->is_ps && sh->version < 0x300)
            {
                /* vs_2_x outputs have no declarations. */
                uint32_t usage = 0xFF, index = 0;
                if (t == RT_RASTOUT && reg <= 2u)
                {
                    static const uint8_t ru[3] = { D3DDECLUSAGE_POSITION, D3DDECLUSAGE_FOG, D3DDECLUSAGE_PSIZE };
                    usage = ru[reg];
                }
                else if (t == RT_ATTROUT && reg < 2u)
                {
                    usage = D3DDECLUSAGE_COLOR;
                    index = reg;
                }
                else if (t == RT_OUTPUT && reg < 8u)
                {
                    usage = D3DDECLUSAGE_TEXCOORD;
                    index = reg;
                }
                if (usage != 0xFF && !find_io(sh->outputs, sh->noutputs, usage, index))
                {
                    /* reg: 0..2 rasterizer outputs, 4..5 colors, 8..15 texture coordinates. */
                    uint32_t r = t == RT_RASTOUT ? reg : (t == RT_ATTROUT ? 4u + reg : 8u + reg);
                    if (add_io(sh->outputs, &sh->noutputs, usage, index, r, 0xF, false, err, errn) == NULL) goto fail;
                }
            }
        }
    }
    if (!end) goto fail;
    if (sh->is_ps && sh->version < 0x200)
    {
        /* Shader model 1 has no declarations: v0 and v1 are the colors, t0
           to t3 the texture coordinates (registers 8 to 11, as in ps_2_x),
           and r0 is the color output. */
        uint32_t k;
        if (add_io(sh->inputs, &sh->ninputs, D3DDECLUSAGE_COLOR, 0, 0, 0xF, false, err, errn) == NULL ||
            add_io(sh->inputs, &sh->ninputs, D3DDECLUSAGE_COLOR, 1, 1, 0xF, false, err, errn) == NULL)
            goto fail;
        for (k = 0; k < 4u; k++)
            if (add_io(sh->inputs, &sh->ninputs, D3DDECLUSAGE_TEXCOORD, k, 8u + k, 0xF, false, err, errn) == NULL)
                goto fail;
        sh->color_outputs = 1;
    }
    if (!sh->is_ps && !find_io(sh->outputs, sh->noutputs, D3DDECLUSAGE_POSITION, 0))
    {
        snprintf(err, errn, "vertex shader writes no position");
        goto fail;
    }
    return true;
bad_len:
    snprintf(err, errn, "instruction %u at token %u is too short", in.op, in.pos);
fail:
    d3d_shader_free(sh);
    return false;
}

/* ---------------------------------------------------------------- code generation */

typedef struct gen {
    const d3d_shader *sh;
    d3dsh_text *out;
    char *err;
    size_t errn;
    bool failed;
    int indent, loop_depth, call_depth;
    uint16_t depth_compare;
    bool sm1;                         /* ps_1_1 to ps_1_3 */
    bool co_defer;                    /* assign writes to "co" (first instruction of a co-issued pair) */
    uint8_t stype[D3DSH_MAX_SAMPLERS]; /* sampler types (D3DSTT_*) of this variant */
    int8_t proj[D3DSH_MAX_SAMPLERS];  /* shader model 1: projection divisor component, -1 = none */
    uint32_t labels[MAX_LABELS]; /* token position of each label instruction, 0 = none */
} gen;

static void fail(gen *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(gen *g, const char *fmt, ...)
{
    va_list ap;
    if (g->failed) return;
    g->failed = true;
    va_start(ap, fmt);
    vsnprintf(g->err, g->errn, fmt, ap);
    va_end(ap);
}

static void emit(gen *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(gen *g, const char *fmt, ...)
{
    va_list ap;
    d3dsh_text *t = g->out;
    int n;
    if (g->failed) return;
    for (;;)
    {
        size_t avail = t->cap - t->len;
        int pad = g->indent * 4;
        if (t->s != NULL && avail > (size_t)pad + 1u)
        {
            memset(t->s + t->len, ' ', (size_t)pad);
            va_start(ap, fmt);
            n = vsnprintf(t->s + t->len + (size_t)pad, avail - (size_t)pad, fmt, ap);
            va_end(ap);
            if (n < 0)
            {
                fail(g, "text format error");
                return;
            }
            if ((size_t)n < avail - (size_t)pad)
            {
                t->len += (size_t)pad + (size_t)n;
                return;
            }
        }
        {
            size_t nc = t->cap ? t->cap * 2u : 16384u;
            char *ns;
            if (nc > MAX_SOURCE)
            {
                fail(g, "generated source is larger than %u bytes", MAX_SOURCE);
                return;
            }
            ns = realloc(t->s, nc);
            if (ns == NULL)
            {
                fail(g, "out of memory");
                return;
            }
            t->s = ns;
            t->cap = nc;
            t->s[t->len] = 0;
        }
    }
}

/* Small expression buffers. */
typedef struct expr {
    char s[384];
} expr;

static void ex_set(gen *g, expr *e, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ex_set(gen *g, expr *e, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(e->s, sizeof e->s, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof e->s) fail(g, "expression too long");
}

static const char comp[4] = { 'x', 'y', 'z', 'w' };

/* Source parameter reader: *i is the index into in->p; returns the token
   and the relative address token (0 if none). */
static bool read_src(gen *g, const inst *in, uint32_t *i, uint32_t *tok, uint32_t *rel)
{
    if (*i >= in->n)
    {
        fail(g, "instruction %u at token %u has too few parameters", in->op, in->pos);
        return false;
    }
    *tok = in->p[(*i)++];
    *rel = 0;
    if (*tok & REL_BIT)
    {
        if (*i >= in->n)
        {
            fail(g, "relative address token missing at token %u", in->pos);
            return false;
        }
        *rel = in->p[(*i)++];
    }
    return true;
}

/* Name of a register read as float4 (no swizzle or modifier). */
static void reg_name(gen *g, uint32_t tok, uint32_t rel, expr *e)
{
    const d3d_shader *sh = g->sh;
    uint32_t t = rtype(tok), n = rnum(tok);
    switch (t)
    {
    case RT_TEMP: ex_set(g, e, "r%u", n); break;
    case RT_INPUT:
        if (rel) fail(g, "relative addressing of input registers is not supported");
        ex_set(g, e, "v%u", n);
        break;
    case RT_ADDR:
        if (g->sm1)
            ex_set(g, e, "tr%u", n); /* texture register t# (ps_1_x) */
        else if (sh->is_ps)
            ex_set(g, e, "v%u", n + 8u); /* t# (ps_2_x) */
        else
            ex_set(g, e, "float4(a0)");
        break;
    case RT_CONST: {
        uint32_t limit = sh->is_ps ? D3DSH_PS_CONST_F : D3DSH_VS_CONST_F;
        if (rel)
        {
            uint32_t rt = rtype(rel), sel = (rel >> 16) & 3u;
            if (sh->nconst_f != limit)
                fail(g, "internal error: the constant scan missed relative addressing at c%u", n);
            if (rt == RT_ADDR)
                ex_set(g, e, "c[clamp(a0.%c + %u, 0, %u)]", comp[sel], n, limit - 1u);
            else if (rt == RT_LOOP)
                ex_set(g, e, "c[clamp(aL + %u, 0, %u)]", n, limit - 1u);
            else
                fail(g, "relative address register type %u is not supported", rt);
        }
        else
        {
            if (n >= limit) fail(g, "constant c%u is out of range", n);
            if (n >= sh->nconst_f) fail(g, "internal error: the constant scan missed c%u", n);
            if (g->sm1)
                ex_set(g, e, "clamp(c[%u], -1.0, 1.0)", n); /* ps_1_x constants are in -1..1 */
            else
                ex_set(g, e, "c[%u]", n);
        }
        break;
    }
    case RT_CONSTINT:
        if (n >= D3DSH_CONST_I) fail(g, "constant i%u is out of range", n);
        ex_set(g, e, "float4(ci[%u])", n);
        break;
    case RT_CONSTBOOL:
        if (n >= D3DSH_CONST_B) fail(g, "constant b%u is out of range", n);
        ex_set(g, e, "float4(cb[%u] != 0u ? 1.0 : 0.0)", n);
        break;
    case RT_LOOP: ex_set(g, e, "float4(float(aL))"); break;
    case RT_MISCTYPE:
        if (n == 0) ex_set(g, e, "vpos");
        else if (n == 1) ex_set(g, e, "float4(vface)");
        else fail(g, "misc register %u", n);
        break;
    case RT_TEMPFLOAT16: ex_set(g, e, "r%u", n); break;
    default: fail(g, "source register type %u is not supported", t); break;
    }
}

/* Full source expression with swizzle and modifier (float4). */
static void src_expr(gen *g, uint32_t tok, uint32_t rel, expr *e)
{
    expr base;
    uint32_t sw = (tok >> 16) & 0xFFu, mod = (tok >> 24) & 0xFu;
    char swz[6] = "";
    reg_name(g, tok, rel, &base);
    if (sw != 0xE4u)
    {
        swz[0] = '.';
        swz[1] = comp[sw & 3u];
        swz[2] = comp[(sw >> 2) & 3u];
        swz[3] = comp[(sw >> 4) & 3u];
        swz[4] = comp[(sw >> 6) & 3u];
        swz[5] = 0;
    }
    switch (mod)
    {
    case 0: ex_set(g, e, "%s%s", base.s, swz); break;
    case 1: ex_set(g, e, "(-%s%s)", base.s, swz); break;
    /* Shader model 1 modifiers. */
    case 2: ex_set(g, e, "(%s%s - 0.5)", base.s, swz); break;            /* _bias */
    case 3: ex_set(g, e, "(0.5 - %s%s)", base.s, swz); break;            /* -x_bias */
    case 4: ex_set(g, e, "(2.0 * %s%s - 1.0)", base.s, swz); break;      /* _bx2 */
    case 5: ex_set(g, e, "(1.0 - 2.0 * %s%s)", base.s, swz); break;      /* -x_bx2 */
    case 6: ex_set(g, e, "(1.0 - %s%s)", base.s, swz); break;            /* 1-x */
    case 7: ex_set(g, e, "(2.0 * %s%s)", base.s, swz); break;            /* _x2 */
    case 8: ex_set(g, e, "(-2.0 * %s%s)", base.s, swz); break;           /* -x_x2 */
    case 11: ex_set(g, e, "abs(%s%s)", base.s, swz); break;
    case 12: ex_set(g, e, "(-abs(%s%s))", base.s, swz); break;
    default: fail(g, "source modifier %u is not supported", mod); break;
    }
}

static bool next_src(gen *g, const inst *in, uint32_t *i, expr *e)
{
    uint32_t tok, rel;
    if (!read_src(g, in, i, &tok, &rel)) return false;
    src_expr(g, tok, rel, e);
    return !g->failed;
}

/* Destination register name (float4 lvalue) and its write mask. */
static void dst_name(gen *g, uint32_t tok, expr *e)
{
    const d3d_shader *sh = g->sh;
    uint32_t t = rtype(tok), n = rnum(tok);
    if (tok & REL_BIT) fail(g, "relative addressing of a destination is not supported");
    switch (t)
    {
    case RT_TEMP:
    case RT_TEMPFLOAT16: ex_set(g, e, "r%u", n); break;
    case RT_ADDR:
        if (g->sm1)
            ex_set(g, e, "tr%u", n);
        else if (sh->is_ps)
            fail(g, "pixel shader writes a texture register");
        else
            ex_set(g, e, "a0");
        break;
    case RT_RASTOUT: ex_set(g, e, "orast%u", n); break;
    case RT_ATTROUT: ex_set(g, e, "oattr%u", n); break;
    case RT_OUTPUT: ex_set(g, e, sh->version >= 0x300 ? "o%u" : "otex%u", n); break;
    case RT_COLOROUT:
        if (n >= 4u) fail(g, "color output %u", n);
        ex_set(g, e, "oC%u", n);
        break;
    case RT_DEPTHOUT: ex_set(g, e, "oDepth"); break;
    case RT_PREDICATE: ex_set(g, e, "p0"); break;
    default: fail(g, "destination register type %u is not supported", t); break;
    }
}

static void mask_str(uint32_t m, char out[6])
{
    int k = 0, c;
    out[k++] = '.';
    for (c = 0; c < 4; c++)
        if (m & (1u << c)) out[k++] = comp[c];
    out[k] = 0;
}

/* Assign a float4 expression to the destination of instruction in. */
static void assign(gen *g, const inst *in, uint32_t dtok, uint32_t ptok, const char *value)
{
    expr d;
    char m[6];
    uint32_t mask = wmask(dtok), t = rtype(dtok);
    bool sat = (dtok & DST_SAT) != 0;
    uint32_t shift = (dtok >> 24) & 0xFu;
    expr sv;
    dst_name(g, dtok, &d);
    if (shift != 0)
    {
        /* Destination shift (ps_1_x): 1 _x2, 2 _x4, 3 _x8, 15 _d2, 14 _d4,
           13 _d8; it comes before the saturation. */
        static const char *const scale[16] = { NULL, "2.0", "4.0", "8.0", NULL, NULL, NULL, NULL,
                                               NULL, NULL, NULL, NULL, NULL, "0.125", "0.25", "0.5" };
        if (!g->sm1 || scale[shift] == NULL)
        {
            fail(g, "destination shift %u is not supported", shift);
            return;
        }
        ex_set(g, &sv, "((%s) * %s)", value, scale[shift]);
        value = sv.s;
    }
    if (g->co_defer) ex_set(g, &d, "co");
    if (mask == 0) return;
    mask_str(mask, m);
    if (t == RT_DEPTHOUT)
    {
        emit(g, "oDepth = %s(%s).x;\n", sat ? "saturate" : "", value);
        return;
    }
    if (t == RT_ADDR)
    {
        emit(g, "a0%s = int4(floor((%s) + 0.5))%s;\n", m, value, m);
        return;
    }
    if (t == RT_PREDICATE)
    {
        fail(g, "predicate written by instruction %u", in->op);
        return;
    }
    if (in->pred)
    {
        expr p;
        uint32_t sw = (ptok >> 16) & 0xFFu;
        char ps[6];
        bool notp = ((ptok >> 24) & 0xFu) == 13u;
        ps[0] = '.';
        ps[1] = comp[sw & 3u];
        ps[2] = comp[(sw >> 2) & 3u];
        ps[3] = comp[(sw >> 4) & 3u];
        ps[4] = comp[(sw >> 6) & 3u];
        ps[5] = 0;
        ex_set(g, &p, "%sp0%s", notp ? "!" : "", ps);
        emit(g, "%s%s = select(%s, %s(%s), %s)%s;\n", d.s, m, d.s, sat ? "saturate" : "", value, p.s, m);
        return;
    }
    if (mask == 0xFu)
        emit(g, "%s = %s(%s);\n", d.s, sat ? "saturate" : "", value);
    else
        emit(g, "%s%s = %s(%s)%s;\n", d.s, m, sat ? "saturate" : "", value, m);
}

static const char *cmp_op(gen *g, uint32_t ctl)
{
    switch (ctl)
    {
    case 1: return ">";
    case 2: return "==";
    case 3: return ">=";
    case 4: return "<";
    case 5: return "!=";
    case 6: return "<=";
    default: fail(g, "comparison %u", ctl); return "==";
    }
}

/* Texture sample. coord: float4 expression; stok: sampler token; projc:
   the component that divides the coordinate, -1 = none.
   D3DSAMP_MIPMAPLODBIAS (lb, from the misc buffer) is added to the level of
   detail, because Metal samplers have no LOD bias: bias(lb) for a plain
   sample, bias(w + lb) for texldb, level(w + lb) for texldl (UNCONFIRMED:
   that D3D9 adds the sampler bias to the texldl level), and gradients
   multiplied by 2^lb for texldd (the same level of detail). A depth
   comparison sample has no bias; the draw stops if the bias is not 0 and
   the depth texture has more than one level (rt_d3d9_draw.m). */
static void tex_sample(gen *g, const inst *in, uint32_t dtok, uint32_t ptok, const char *coord, uint32_t stok,
                       int projc, const char *grad_x, const char *grad_y)
{
    uint32_t s = rnum(stok), st, sw = (stok >> 16) & 0xFFu;
    bool proj = projc >= 0, bias = !g->sm1 && in->op == OP_TEX && in->ctl == 2;
    bool lod = in->op == OP_TEXLDL, grad = in->op == OP_TEXLDD;
    char pc = proj ? comp[projc & 3] : 'w';
    const char *tp = g->sh->is_ps ? "" : "v";
    expr c, v, r;
    char swz[6] = "", lb[32];
    if (rtype(stok) != RT_SAMPLER || s >= D3DSH_MAX_SAMPLERS || g->stype[s] == 0)
    {
        fail(g, "texture instruction at token %u uses an undeclared sampler", in->pos);
        return;
    }
    st = g->stype[s];
    snprintf(lb, sizeof lb, "%s_misc[%u].%c", g->sh->is_ps ? "ps" : "vs", 1u + s / 4u, comp[s & 3u]);
    if (sw != 0xE4u)
    {
        swz[0] = '.';
        swz[1] = comp[sw & 3u];
        swz[2] = comp[(sw >> 2) & 3u];
        swz[3] = comp[(sw >> 4) & 3u];
        swz[4] = comp[(sw >> 6) & 3u];
        swz[5] = 0;
    }
    if (proj)
        ex_set(g, &c, st == D3DSTT_2D ? "((%s).xy / (%s).%c)" : "((%s).xyz / (%s).%c)", coord, coord, pc);
    else
        ex_set(g, &c, st == D3DSTT_2D ? "(%s).xy" : "(%s).xyz", coord);
    if (g->depth_compare & (1u << s))
    {
        if (st != D3DSTT_2D || lod || grad || bias)
        {
            fail(g, "depth comparison sampler s%u is not a plain 2D sample", s);
            return;
        }
        if (proj)
            ex_set(g, &r, "((%s).z / (%s).%c)", coord, coord, pc);
        else
            ex_set(g, &r, "(%s).z", coord);
        ex_set(g, &v, "float4(%st%u.sample_compare(%ss%u, %s, %s))%s", tp, s, tp, s, c.s, r.s, swz);
    }
    else if (lod)
        ex_set(g, &v, "%st%u.sample(%ss%u, %s, level((%s).w + %s))%s", tp, s, tp, s, c.s, coord, lb, swz);
    else if (bias)
        ex_set(g, &v, "%st%u.sample(%ss%u, %s, bias((%s).w + %s))%s", tp, s, tp, s, c.s, coord, lb, swz);
    else if (grad)
    {
        const char *gf = st == D3DSTT_2D ? "gradient2d" : (st == D3DSTT_CUBE ? "gradientcube" : "gradient3d");
        const char *gc = st == D3DSTT_2D ? "xy" : "xyz";
        ex_set(g, &v, "%st%u.sample(%ss%u, %s, %s((%s).%s * exp2(%s), (%s).%s * exp2(%s)))%s", tp, s, tp, s, c.s, gf,
               grad_x, gc, lb, grad_y, gc, lb, swz);
    }
    else
    {
        if (!g->sh->is_ps)
        {
            fail(g, "vertex shader texture instruction without a level (texldl is required)");
            return;
        }
        ex_set(g, &v, "t%u.sample(s%u, %s, bias(%s))%s", s, s, c.s, lb, swz);
    }
    assign(g, in, dtok, ptok, v.s);
}

static uint32_t matrix_rows(uint32_t op)
{
    switch (op)
    {
    case OP_M4x4: return 4;
    case OP_M4x3: return 3;
    case OP_M3x4: return 4;
    case OP_M3x3: return 3;
    case OP_M3x2: return 2;
    default: return 0;
    }
}

static void gen_block(gen *g, uint32_t pos);

static void gen_inst(gen *g, const inst *in)
{
    uint32_t i = 0, dtok = 0, ptok = 0;
    expr a, b, c, v;
    switch (in->op)
    {
    case OP_NOP:
    case OP_DCL:
    case OP_DEF:
    case OP_DEFI:
    case OP_DEFB: return;
    case OP_PHASE: fail(g, "phase instruction (ps_1_4) is not supported"); return;
    case OP_ELSE:
        g->indent--;
        emit(g, "} else {\n");
        g->indent++;
        return;
    case OP_ENDIF:
    case OP_ENDREP:
        g->indent--;
        emit(g, "}\n");
        if (in->op == OP_ENDREP) g->loop_depth--;
        return;
    case OP_ENDLOOP:
        g->indent--;
        emit(g, "}\n");
        emit(g, "aL = aL_save%d;\n", g->loop_depth);
        g->indent--;
        emit(g, "}\n");
        g->loop_depth--;
        return;
    case OP_BREAK: emit(g, "break;\n"); return;
    case OP_IF: {
        uint32_t tok, rel;
        if (!read_src(g, in, &i, &tok, &rel)) return;
        if (rtype(tok) == RT_CONSTBOOL)
        {
            bool notb = ((tok >> 24) & 0xFu) == 13u;
            if (rnum(tok) >= D3DSH_CONST_B) fail(g, "b%u out of range", rnum(tok));
            emit(g, "if (cb[%u] %s 0u) {\n", rnum(tok), notb ? "==" : "!=");
        }
        else if (rtype(tok) == RT_PREDICATE)
        {
            bool notp = ((tok >> 24) & 0xFu) == 13u;
            emit(g, "if (%sp0.%c) {\n", notp ? "!" : "", comp[(tok >> 16) & 3u]);
        }
        else
            fail(g, "if on register type %u", rtype(tok));
        g->indent++;
        return;
    }
    case OP_IFC:
    case OP_BREAKC: {
        const char *op = cmp_op(g, in->ctl);
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        if (in->op == OP_IFC)
        {
            emit(g, "if ((%s).x %s (%s).x) {\n", a.s, op, b.s);
            g->indent++;
        }
        else
            emit(g, "if ((%s).x %s (%s).x) break;\n", a.s, op, b.s);
        return;
    }
    case OP_BREAKP: {
        uint32_t tok, rel;
        if (!read_src(g, in, &i, &tok, &rel)) return;
        emit(g, "if (%sp0.%c) break;\n", ((tok >> 24) & 0xFu) == 13u ? "!" : "", comp[(tok >> 16) & 3u]);
        return;
    }
    case OP_REP: {
        uint32_t tok, rel;
        if (!read_src(g, in, &i, &tok, &rel)) return;
        if (rtype(tok) != RT_CONSTINT || rnum(tok) >= D3DSH_CONST_I)
        {
            fail(g, "rep on a register that is not i#");
            return;
        }
        g->loop_depth++;
        emit(g, "for (int rep%d = 0; rep%d < min(ci[%u].x, 255); rep%d++) {\n", g->loop_depth, g->loop_depth,
             rnum(tok), g->loop_depth);
        g->indent++;
        return;
    }
    case OP_LOOP: {
        uint32_t ltok, lrel, tok, rel, n;
        if (!read_src(g, in, &i, &ltok, &lrel) || !read_src(g, in, &i, &tok, &rel)) return;
        if (rtype(tok) != RT_CONSTINT || rnum(tok) >= D3DSH_CONST_I)
        {
            fail(g, "loop on a register that is not i#");
            return;
        }
        n = rnum(tok);
        g->loop_depth++;
        emit(g, "{\n");
        g->indent++;
        emit(g, "int aL_save%d = aL;\n", g->loop_depth);
        emit(g, "aL = ci[%u].y;\n", n);
        emit(g, "for (int lp%d = 0; lp%d < min(ci[%u].x, 255); lp%d++, aL += ci[%u].z) {\n", g->loop_depth,
             g->loop_depth, n, g->loop_depth, n);
        g->indent++;
        return;
    }
    case OP_CALL:
    case OP_CALLNZ: {
        uint32_t ltok, lrel, l;
        if (!read_src(g, in, &i, &ltok, &lrel)) return;
        l = rnum(ltok);
        if (l >= MAX_LABELS || g->labels[l] == 0)
        {
            fail(g, "call of an undefined label %u", l);
            return;
        }
        if (g->call_depth >= MAX_CALL_DEPTH)
        {
            fail(g, "call depth is more than %d", MAX_CALL_DEPTH);
            return;
        }
        if (in->op == OP_CALLNZ)
        {
            uint32_t tok, rel;
            if (!read_src(g, in, &i, &tok, &rel)) return;
            if (rtype(tok) == RT_CONSTBOOL)
                emit(g, "if (cb[%u] %s 0u) {\n", rnum(tok) & 15u, ((tok >> 24) & 0xFu) == 13u ? "==" : "!=");
            else if (rtype(tok) == RT_PREDICATE)
                emit(g, "if (%sp0.%c) {\n", ((tok >> 24) & 0xFu) == 13u ? "!" : "", comp[(tok >> 16) & 3u]);
            else
            {
                fail(g, "callnz on register type %u", rtype(tok));
                return;
            }
        }
        else
            emit(g, "{\n");
        g->indent++;
        g->call_depth++;
        gen_block(g, g->labels[l]);
        g->call_depth--;
        g->indent--;
        emit(g, "}\n");
        return;
    }
    case OP_TEXKILL: {
        uint32_t t = in->n > 0 ? in->p[0] : 0, m = wmask(t), k;
        expr r;
        char cond[96] = "";
        size_t len = 0;
        if (in->n < 1)
        {
            fail(g, "texkill without operand");
            return;
        }
        if (rtype(t) == RT_TEMP)
            ex_set(g, &r, "r%u", rnum(t));
        else if (rtype(t) == RT_ADDR && g->sh->is_ps)
            ex_set(g, &r, "v%u", rnum(t) + 8u);
        else if (rtype(t) == RT_INPUT)
            ex_set(g, &r, "v%u", rnum(t));
        else
        {
            fail(g, "texkill on register type %u", rtype(t));
            return;
        }
        if (g->sm1)
            m = 0x7; /* ps_1_x: the u, v and w of the texture coordinate */
        else if (m == 0)
            m = 0xF;
        for (k = 0; k < 4; k++)
        {
            if (!(m & (1u << k))) continue;
            len += (size_t)snprintf(cond + len, sizeof cond - len, "%s%s.%c < 0.0", len ? " || " : "", r.s, comp[k]);
        }
        emit(g, "if (%s) discard_fragment();\n", cond);
        return;
    }
    case OP_SETP: {
        expr d;
        char m[6];
        uint32_t dt;
        if (in->n < 1) return;
        dt = in->p[i++];
        if (rtype(dt) != RT_PREDICATE)
        {
            fail(g, "setp destination is not p0");
            return;
        }
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        mask_str(wmask(dt), m);
        dst_name(g, dt, &d);
        emit(g, "p0%s = ((%s) %s (%s))%s;\n", m, a.s, cmp_op(g, in->ctl), b.s, m);
        return;
    }
    default: break;
    }

    /* Instructions with a destination. */
    if (in->n < 1)
    {
        fail(g, "instruction %u at token %u has no destination", in->op, in->pos);
        return;
    }
    dtok = in->p[i++];
    if (in->pred)
    {
        if (i >= in->n)
        {
            fail(g, "predicate token missing");
            return;
        }
        ptok = in->p[i++];
    }
    switch (in->op)
    {
    case OP_MOV:
    case OP_MOVA:
        if (next_src(g, in, &i, &a)) assign(g, in, dtok, ptok, a.s);
        return;
    case OP_ADD:
    case OP_SUB:
    case OP_MUL:
    case OP_MIN:
    case OP_MAX:
    case OP_SLT:
    case OP_SGE:
    case OP_DP3:
    case OP_DP4:
    case OP_CRS:
    case OP_DST:
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        switch (in->op)
        {
        case OP_ADD: ex_set(g, &v, "%s + %s", a.s, b.s); break;
        case OP_SUB: ex_set(g, &v, "%s - %s", a.s, b.s); break;
        case OP_MUL: ex_set(g, &v, "%s * %s", a.s, b.s); break;
        case OP_MIN: ex_set(g, &v, "min(%s, %s)", a.s, b.s); break;
        case OP_MAX: ex_set(g, &v, "max(%s, %s)", a.s, b.s); break;
        case OP_SLT: ex_set(g, &v, "select(float4(0.0), float4(1.0), %s < %s)", a.s, b.s); break;
        case OP_SGE: ex_set(g, &v, "select(float4(0.0), float4(1.0), %s >= %s)", a.s, b.s); break;
        case OP_DP3: ex_set(g, &v, "float4(dot((%s).xyz, (%s).xyz))", a.s, b.s); break;
        case OP_DP4: ex_set(g, &v, "float4(dot(%s, %s))", a.s, b.s); break;
        case OP_CRS: ex_set(g, &v, "float4(cross((%s).xyz, (%s).xyz), 0.0)", a.s, b.s); break;
        default: ex_set(g, &v, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a.s, b.s, a.s, b.s); break;
        }
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_MAD:
    case OP_LRP:
    case OP_CMP:
    case OP_CND:
    case OP_DP2ADD:
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b) || !next_src(g, in, &i, &c)) return;
        if (in->op == OP_CND)
            /* ps_1_1 to ps_1_3 require r0.a as the condition (replicated). */
            ex_set(g, &v, "select(%s, %s, %s > 0.5)", c.s, b.s, a.s);
        else if (in->op == OP_MAD)
            ex_set(g, &v, "fma(%s, %s, %s)", a.s, b.s, c.s);
        else if (in->op == OP_LRP)
            ex_set(g, &v, "mix(%s, %s, %s)", c.s, b.s, a.s);
        else if (in->op == OP_CMP)
            ex_set(g, &v, "select(%s, %s, %s >= 0.0)", c.s, b.s, a.s);
        else
            ex_set(g, &v, "float4(dot((%s).xy, (%s).xy) + (%s).x)", a.s, b.s, c.s);
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_RCP:
    case OP_RSQ:
    case OP_EXP:
    case OP_EXPP:
    case OP_LOG:
    case OP_LOGP:
    case OP_FRC:
    case OP_ABS:
    case OP_NRM:
    case OP_SGN:
    case OP_LIT:
    case OP_DSX:
    case OP_DSY:
    case OP_SINCOS:
        if (!next_src(g, in, &i, &a)) return;
        switch (in->op)
        {
        case OP_RCP: ex_set(g, &v, "float4(1.0 / (%s).x)", a.s); break;
        case OP_RSQ: ex_set(g, &v, "float4(rsqrt(abs((%s).x)))", a.s); break;
        case OP_EXP:
        case OP_EXPP: ex_set(g, &v, "float4(exp2((%s).x))", a.s); break;
        case OP_LOG:
        case OP_LOGP: ex_set(g, &v, "float4(log2(abs((%s).x)))", a.s); break;
        case OP_FRC: ex_set(g, &v, "fract(%s)", a.s); break;
        case OP_ABS: ex_set(g, &v, "abs(%s)", a.s); break;
        case OP_NRM: ex_set(g, &v, "((%s) * rsqrt(dot((%s).xyz, (%s).xyz)))", a.s, a.s, a.s); break;
        case OP_SGN: ex_set(g, &v, "sign(%s)", a.s); break;
        case OP_LIT: ex_set(g, &v, "d3d_lit(%s)", a.s); break;
        case OP_DSX: ex_set(g, &v, "dfdx(%s)", a.s); break;
        case OP_DSY: ex_set(g, &v, "dfdy(%s)", a.s); break;
        default: ex_set(g, &v, "float4(cos((%s).x), sin((%s).x), 0.0, 0.0)", a.s, a.s); break;
        }
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_POW:
        if (!next_src(g, in, &i, &a) || !next_src(g, in, &i, &b)) return;
        ex_set(g, &v, "float4(pow(abs((%s).x), (%s).x))", a.s, b.s);
        assign(g, in, dtok, ptok, v.s);
        return;
    case OP_M4x4:
    case OP_M4x3:
    case OP_M3x4:
    case OP_M3x3:
    case OP_M3x2: {
        uint32_t mtok, mrel, rows = matrix_rows(in->op), k;
        bool four = in->op == OP_M4x4 || in->op == OP_M4x3;
        char parts[4][384];
        if (!next_src(g, in, &i, &a) || !read_src(g, in, &i, &mtok, &mrel)) return;
        for (k = 0; k < 4; k++)
        {
            if (k < rows)
            {
                uint32_t t2 = (mtok & ~0x7FFu) | ((rnum(mtok) + k) & 0x7FFu);
                expr m;
                src_expr(g, t2, mrel, &m);
                snprintf(parts[k], sizeof parts[k], four ? "dot(%s, %s)" : "dot((%s).xyz, (%s).xyz)", a.s, m.s);
            }
            else
                snprintf(parts[k], sizeof parts[k], "0.0");
        }
        ex_set(g, &v, "float4(%s, %s, %s, %s)", parts[0], parts[1], parts[2], parts[3]);
        assign(g, in, dtok, ptok, v.s);
        return;
    }
    case OP_TEX:
    case OP_TEXLDL: {
        uint32_t stok, srel;
        if (g->sm1)
        {
            /* tex t#: sampler # at TEXCOORD# (v8 + #). */
            uint32_t n = rnum(dtok);
            stok = (2u << 28) | (1u << 11) | (0xE4u << 16) | n; /* s# */
            ex_set(g, &a, "v%u", 8u + n);
            tex_sample(g, in, dtok, ptok, a.s, stok, g->proj[n], NULL, NULL);
            return;
        }
        if (!next_src(g, in, &i, &a) || !read_src(g, in, &i, &stok, &srel)) return;
        tex_sample(g, in, dtok, ptok, a.s, stok, in->op == OP_TEX && in->ctl == 1 ? 3 : -1, NULL, NULL);
        return;
    }
    case OP_TEXCOORD: {
        /* texcoord t#: TEXCOORD# clamped to 0..1, with w = 1. */
        if (!g->sm1)
        {
            fail(g, "texcoord in a shader of model 2 or later");
            return;
        }
        ex_set(g, &v, "float4(saturate(v%u.xyz), 1.0)", 8u + rnum(dtok));
        assign(g, in, dtok, ptok, v.s);
        return;
    }
    case OP_TEXLDD: {
        uint32_t stok, srel;
        if (!next_src(g, in, &i, &a) || !read_src(g, in, &i, &stok, &srel) || !next_src(g, in, &i, &b) ||
            !next_src(g, in, &i, &c))
            return;
        tex_sample(g, in, dtok, ptok, a.s, stok, -1, b.s, c.s);
        return;
    }
    default: fail(g, "opcode %u at token %u is not supported", in->op, in->pos); return;
    }
}

/* A co-issued pair (shader model 1): both instructions read their sources
   before either writes. The first one writes to "co"; the masked
   components go to its destination after the second one. */
static void gen_coissue(gen *g, const inst *first, const inst *second)
{
    expr d;
    char m[6];
    uint32_t dtok;
    if (first->n < 1 || second->n < 1 || first->op == OP_TEX || first->op == OP_TEXCOORD ||
        first->op == OP_TEXKILL || first->op == OP_DEF || second->op == OP_TEX || second->op == OP_TEXCOORD ||
        second->op == OP_TEXKILL || second->op == OP_DEF)
    {
        fail(g, "co-issued instructions %u and %u at token %u are not arithmetic instructions", first->op, second->op,
             second->pos);
        return;
    }
    dtok = first->p[0];
    dst_name(g, dtok, &d);
    mask_str(wmask(dtok), m);
    emit(g, "{\n");
    g->indent++;
    emit(g, "float4 co = float4(0.0);\n");
    g->co_defer = true;
    gen_inst(g, first);
    g->co_defer = false;
    gen_inst(g, second);
    if (wmask(dtok) != 0) emit(g, "%s%s = co%s;\n", d.s, m, m);
    g->indent--;
    emit(g, "}\n");
}

/* Emit instructions from token position pos until ret or the end token. */
static void gen_block(gen *g, uint32_t pos)
{
    inst in;
    bool end;
    while (!g->failed && next_inst(g->sh->tokens, g->sh->ntokens, &pos, &in, &end, g->err, g->errn))
    {
        if (in.op == OP_RET || in.op == OP_LABEL) return;
        if (in.coissue)
        {
            fail(g, "co-issued instruction at token %u has no instruction before it", in.pos);
            return;
        }
        if (g->sm1)
        {
            uint32_t p2 = pos;
            inst nx;
            bool e2;
            if (next_inst(g->sh->tokens, g->sh->ntokens, &p2, &nx, &e2, g->err, g->errn) && nx.coissue)
            {
                gen_coissue(g, &in, &nx);
                pos = p2;
                continue;
            }
        }
        gen_inst(g, &in);
    }
    if (!end && !g->failed) fail(g, "shader token stream is malformed");
}

static bool gen_init(gen *g, const d3d_shader *sh, d3dsh_text *out, char *err, size_t errn)
{
    uint32_t pos = 1;
    inst in;
    bool end;
    memset(g, 0, sizeof *g);
    g->sh = sh;
    g->out = out;
    g->err = err;
    g->errn = errn;
    g->sm1 = sh->is_ps && sh->version < 0x200;
    memcpy(g->stype, sh->sampler_type, sizeof g->stype);
    memset(g->proj, -1, sizeof g->proj);
    memset(out, 0, sizeof *out);
    while (next_inst(sh->tokens, sh->ntokens, &pos, &in, &end, err, errn))
    {
        if (in.op == OP_LABEL && in.n >= 1)
        {
            uint32_t l = rnum(in.p[0]);
            if (l >= MAX_LABELS)
            {
                snprintf(err, errn, "label %u is out of range", l);
                return false;
            }
            g->labels[l] = pos; /* body starts after the label instruction */
        }
    }
    emit(g, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    emit(g, "static inline float4 d3d_lit(float4 s)\n{\n"
            "    float4 d = float4(1.0, 0.0, 0.0, 1.0);\n"
            "    if (s.x > 0.0) {\n"
            "        d.y = s.x;\n"
            "        if (s.y > 0.0) d.z = pow(s.y, clamp(s.w, -128.0, 128.0));\n"
            "    }\n"
            "    return d;\n}\n\n");
    /* The value that an 8-bit (unorm) target stores, for 8-bit targets in
       32-bit float storage (d3dsh_unorm): 0..1, NaN and -infinity give 0,
       +infinity gives 1. The test uses the bits, because the shaders are
       compiled with fast math, which can remove a floating-point NaN test. */
    emit(g, "static inline float4 d3d_unorm(float4 v)\n{\n"
            "    uint4 b = as_type<uint4>(v);\n"
            "    v = select(v, select(float4(0.0), float4(1.0), b == 0x7f800000u), (b & 0x7f800000u) == 0x7f800000u);\n"
            "    return clamp(v, 0.0, 1.0);\n}\n\n");
    /* D3DCMPFUNC: 1 never, 2 less, 3 equal, 4 less-equal, 5 greater,
       6 not-equal, 7 greater-equal, 8 always. */
    emit(g, "static inline bool d3d_alpha(int f, float a, float r)\n{\n"
            "    switch (f) {\n"
            "    case 1: return false;\n"
            "    case 2: return a < r;\n"
            "    case 3: return a == r;\n"
            "    case 4: return a <= r;\n"
            "    case 5: return a > r;\n"
            "    case 6: return a != r;\n"
            "    case 7: return a >= r;\n"
            "    default: return true;\n"
            "    }\n}\n\n");
    return !g->failed;
}

static void declare_temps(gen *g)
{
    int k;
    for (k = 0; k < 32; k++)
        emit(g, "float4 r%d = float4(0.0);\n", k);
    emit(g, "int4 a0 = int4(0);\n");
    emit(g, "int aL = 0;\n");
    emit(g, "bool4 p0 = bool4(false);\n");
}

static bool finish(gen *g, d3dsh_text *out)
{
    if (g->failed)
    {
        d3dsh_text_free(out);
        return false;
    }
    return true;
}

/* Name of the local variable that holds output entry e of a vertex shader. */
static void vs_out_name(gen *g, const d3d_shader *vs, const d3dsh_io *e, expr *x)
{
    if (vs->version >= 0x300)
        ex_set(g, x, "o%u", e->reg);
    else if (e->reg < 4u)
        ex_set(g, x, "orast%u", e->reg);
    else if (e->reg < 8u)
        ex_set(g, x, "oattr%u", e->reg - 4u);
    else
        ex_set(g, x, "otex%u", e->reg - 8u);
}

static bool is_special_vs_output(const d3dsh_io *e)
{
    return (e->usage == D3DDECLUSAGE_POSITION || e->usage == D3DDECLUSAGE_PSIZE) && e->index == 0;
}

static void texture_params(gen *g, const d3d_shader *sh, uint16_t depth_compare)
{
    uint32_t s;
    const char *tp = sh->is_ps ? "" : "v";
    for (s = 0; s < D3DSH_MAX_SAMPLERS; s++)
    {
        const char *type;
        switch (g->stype[s])
        {
        case 0: continue;
        case D3DSTT_2D: type = (depth_compare & (1u << s)) ? "depth2d<float>" : "texture2d<float>"; break;
        case D3DSTT_CUBE: type = "texturecube<float>"; break;
        default: type = "texture3d<float>"; break;
        }
        emit(g, ",\n    %s %st%u [[texture(%u)]], sampler %ss%u [[sampler(%u)]]", type, tp, s, s, tp, s, s);
    }
}

bool d3d_shader_msl_vs(const d3d_shader *vs, d3dsh_text *out, char *err, size_t errn)
{
    gen g;
    int k;
    const d3dsh_io *pos_out = NULL, *psize_out = NULL;
    expr x;
    if (vs->is_ps)
    {
        snprintf(err, errn, "not a vertex shader");
        return false;
    }
    if (!gen_init(&g, vs, out, err, errn)) return finish(&g, out);
    for (k = 0; k < D3DSH_MAX_SAMPLERS; k++)
        if (vs->sampler_type[k] != 0 && k >= 4) fail(&g, "vertex sampler s%d (only s0 to s3)", k);

    if (vs->ninputs > 0)
    {
        emit(&g, "struct VIn {\n");
        for (k = 0; k < vs->ninputs; k++)
            emit(&g, "    float4 a%u [[attribute(%u)]];\n", vs->inputs[k].reg, vs->inputs[k].reg);
        emit(&g, "};\n\n");
    }
    emit(&g, "struct VOut {\n    float4 pos [[position]];\n");
    for (k = 0; k < vs->noutputs; k++)
    {
        const d3dsh_io *e = &vs->outputs[k];
        if (e->usage == D3DDECLUSAGE_POSITION && e->index == 0)
            pos_out = e;
        else if (e->usage == D3DDECLUSAGE_PSIZE && e->index == 0)
        {
            psize_out = e;
            emit(&g, "    float psize [[point_size]];\n");
        }
        else
            emit(&g, "    float4 %s%u [[user(%s%u)]];\n", usage_name(e->usage), e->index, usage_name(e->usage),
                 e->index);
    }
    emit(&g, "};\n\n");
    emit(&g, "vertex VOut vs_main(");
    if (vs->ninputs > 0) emit(&g, "VIn vin [[stage_in]],\n    ");
    emit(&g, "constant float4 *c [[buffer(16)]], constant int4 *ci [[buffer(17)]], constant uint *cb [[buffer(18)]],\n"
             "    constant float4 *vs_misc [[buffer(19)]]");
    texture_params(&g, vs, 0);
    emit(&g, ")\n{\n");
    g.indent = 1;
    for (k = 0; k < vs->ninputs; k++)
        emit(&g, "float4 v%u = vin.a%u;\n", vs->inputs[k].reg, vs->inputs[k].reg);
    declare_temps(&g);
    if (vs->version >= 0x300)
    {
        for (k = 0; k < 12; k++)
            emit(&g, "float4 o%d = float4(0.0);\n", k);
    }
    else
    {
        for (k = 0; k < 3; k++)
            emit(&g, "float4 orast%d = float4(0.0);\n", k);
        for (k = 0; k < 2; k++)
            emit(&g, "float4 oattr%d = float4(0.0);\n", k);
        for (k = 0; k < 8; k++)
            emit(&g, "float4 otex%d = float4(0.0);\n", k);
    }
    gen_block(&g, 1);
    emit(&g, "VOut out;\n");
    if (pos_out == NULL)
        fail(&g, "vertex shader writes no position");
    else
    {
        vs_out_name(&g, vs, pos_out, &x);
        emit(&g, "out.pos = %s;\n", x.s);
        /* D3D9 pixel centers are at integer coordinates. */
        emit(&g, "out.pos.xy += vs_misc[0].xy * out.pos.w;\n");
        /* Constant depth bias (D3DRS_DEPTHBIAS): z/w changes by vs_misc[0].z. */
        emit(&g, "out.pos.z += vs_misc[0].z * out.pos.w;\n");
    }
    if (psize_out != NULL)
    {
        vs_out_name(&g, vs, psize_out, &x);
        /* Point size in D3D pixels; vs_misc[0].w = Metal pixels per D3D pixel. */
        emit(&g, "out.psize = %s.x * vs_misc[0].w;\n", x.s);
    }
    for (k = 0; k < vs->noutputs; k++)
    {
        const d3dsh_io *e = &vs->outputs[k];
        if (is_special_vs_output(e)) continue;
        vs_out_name(&g, vs, e, &x);
        /* Colors of shader model 2 are clamped to 0..1. */
        if (vs->version < 0x300 && e->usage == D3DDECLUSAGE_COLOR)
            emit(&g, "out.%s%u = saturate(%s);\n", usage_name(e->usage), e->index, x.s);
        else
            emit(&g, "out.%s%u = %s;\n", usage_name(e->usage), e->index, x.s);
    }
    emit(&g, "return out;\n");
    g.indent = 0;
    emit(&g, "}\n");
    return finish(&g, out);
}

/* A D3DBLEND factor for blending in the shader (d3dsh_unorm): s = source,
   d = target pixel. NULL if the factor is not supported. */
static const char *blend_factor_msl(uint8_t f, bool alpha)
{
    switch (f)
    {
    case 1: return alpha ? "0.0" : "float3(0.0)";
    case 2: return alpha ? "1.0" : "float3(1.0)";
    case 3: return alpha ? "s.a" : "s.rgb";
    case 4: return alpha ? "(1.0 - s.a)" : "(1.0 - s.rgb)";
    case 5: return alpha ? "s.a" : "float3(s.a)";
    case 6: return alpha ? "(1.0 - s.a)" : "float3(1.0 - s.a)";
    case 7: return alpha ? "d.a" : "float3(d.a)";
    case 8: return alpha ? "(1.0 - d.a)" : "float3(1.0 - d.a)";
    case 9: return alpha ? "d.a" : "d.rgb";
    case 10: return alpha ? "(1.0 - d.a)" : "(1.0 - d.rgb)";
    case 11: return alpha ? "1.0" : "float3(min(s.a, 1.0 - d.a))";
    case 14: return alpha ? "blend_color.a" : "blend_color.rgb";
    case 15: return alpha ? "(1.0 - blend_color.a)" : "(1.0 - blend_color.rgb)";
    default: return NULL;
    }
}

/* The D3DBLENDOP expression (into buf) of the source value sv with factor
   fs and the target value dv with factor fd. MIN and MAX do not use the
   factors (D3DBLENDOP documentation). False if op is not valid. */
static bool blend_op_msl(char *buf, size_t n, uint8_t op, const char *sv, const char *fs, const char *dv,
                         const char *fd)
{
    switch (op)
    {
    case 1: snprintf(buf, n, "%s * %s + %s * %s", sv, fs, dv, fd); return true;
    case 2: snprintf(buf, n, "%s * %s - %s * %s", sv, fs, dv, fd); return true;
    case 3: snprintf(buf, n, "%s * %s - %s * %s", dv, fd, sv, fs); return true;
    case 4: snprintf(buf, n, "min(%s, %s)", sv, dv); return true;
    case 5: snprintf(buf, n, "max(%s, %s)", sv, dv); return true;
    default: return false;
    }
}

bool d3d_shader_msl_ps(const d3d_shader *ps, const d3dsh_io *vs_outputs, int nvs, uint16_t depth_compare,
                       uint16_t sm1_tex, uint16_t as_2d, const d3dsh_unorm *unorm, d3dsh_text *out, char *err,
                       size_t errn)
{
    gen g;
    int k, nmembers = 0;
    bool need_pos;
    if (!ps->is_ps)
    {
        snprintf(err, errn, "not a pixel shader");
        return false;
    }
    if (!gen_init(&g, ps, out, err, errn)) return finish(&g, out);
    g.depth_compare = depth_compare;
    if (g.sm1)
    {
        for (k = 0; k < 4; k++)
        {
            uint32_t type = (sm1_tex >> (8 + 2 * k)) & 3u, div = (sm1_tex >> (2 * k)) & 3u;
            if (ps->sampler_type[k] == 0) continue;
            if (type == 3u) fail(&g, "bad texture type in the shader model 1 variant 0x%04x", sm1_tex);
            g.stype[k] = (uint8_t)(type == 1u ? D3DSTT_CUBE : type == 2u ? D3DSTT_VOLUME : D3DSTT_2D);
            g.proj[k] = (int8_t)(div == 0u ? -1 : (int)div); /* 1 y, 2 z, 3 w */
        }
    }
    else if (sm1_tex != 0)
        fail(&g, "shader model 1 variant 0x%04x for a shader of model 2 or later", sm1_tex);
    if (as_2d != 0)
    {
        if (g.sm1) fail(&g, "2D texture variant 0x%04x for a shader model 1 shader", as_2d);
        for (k = 0; k < D3DSH_MAX_SAMPLERS; k++)
        {
            if (!(as_2d & (1u << k))) continue;
            if (ps->sampler_type[k] != D3DSTT_CUBE && ps->sampler_type[k] != D3DSTT_VOLUME)
                fail(&g, "2D texture variant 0x%04x: sampler s%d is not a cube or volume sampler", as_2d, k);
            g.stype[k] = D3DSTT_2D;
        }
    }
    need_pos = ps->uses_vpos || ps->writes_depth;

    for (k = 0; k < ps->ninputs; k++)
    {
        const d3dsh_io *e = &ps->inputs[k];
        if (find_io(vs_outputs, nvs, e->usage, e->index) && !is_special_vs_output(e))
        {
            if (nmembers++ == 0) emit(&g, "struct PIn {\n");
            emit(&g, "    float4 %s%u [[user(%s%u)%s]];\n", usage_name(e->usage), e->index, usage_name(e->usage),
                 e->index, e->centroid ? ", centroid_perspective" : "");
        }
    }
    if (nmembers > 0) emit(&g, "};\n\n");
    emit(&g, "struct POut {\n");
    for (k = 0; k < 4; k++)
        if (ps->color_outputs & (1u << k)) emit(&g, "    float4 oC%d [[color(%d)]];\n", k, k);
    if (ps->writes_depth) emit(&g, "    float oDepth [[depth(any)]];\n");
    if (ps->color_outputs == 0 && !ps->writes_depth) fail(&g, "pixel shader writes no output");
    emit(&g, "};\n\n");

    emit(&g, "fragment POut ps_main(");
    if (nmembers > 0) emit(&g, "PIn pin [[stage_in]], ");
    if (need_pos) emit(&g, "float4 fpos [[position]], ");
    if (ps->uses_vface) emit(&g, "bool ffront [[front_facing]], ");
    emit(&g, "\n    constant float4 *c [[buffer(16)]], constant int4 *ci [[buffer(17)]], constant uint *cb [[buffer(18)]],\n"
             "    constant float4 *ps_misc [[buffer(19)]]");
    if (unorm->blend & ps->color_outputs)
    {
        emit(&g, ", constant float4 &blend_color [[buffer(20)]]");
        for (k = 0; k < 4; k++)
            if (unorm->blend & ps->color_outputs & (1u << k)) emit(&g, ", float4 dst%d [[color(%d)]]", k, k);
    }
    texture_params(&g, ps, depth_compare);
    emit(&g, ")\n{\n");
    g.indent = 1;
    /* VPOS: the integer D3D pixel; ps_misc[0].z = D3D pixels per Metal pixel. */
    if (ps->uses_vpos) emit(&g, "float4 vpos = float4(floor(fpos.xy * ps_misc[0].z), fpos.z, fpos.w);\n");
    if (ps->uses_vface) emit(&g, "float vface = ffront ? 1.0 : -1.0;\n");
    for (k = 0; k < ps->ninputs; k++)
    {
        const d3dsh_io *e = &ps->inputs[k];
        if (find_io(vs_outputs, nvs, e->usage, e->index) && !is_special_vs_output(e))
            emit(&g, "float4 v%u = pin.%s%u;\n", e->reg, usage_name(e->usage), e->index);
        else
            /* The vertex shader does not write this input; D3D9 leaves it
               undefined. */
            emit(&g, "float4 v%u = float4(0.0);\n", e->reg);
    }
    declare_temps(&g);
    if (g.sm1)
        for (k = 0; k < 4; k++)
            emit(&g, "float4 tr%d = float4(0.0);\n", k);
    for (k = 0; k < 4; k++)
        emit(&g, "float4 oC%d = float4(0.0);\n", k);
    if (ps->writes_depth) emit(&g, "float oDepth = fpos.z;\n");
    gen_block(&g, 1);
    if (g.sm1) emit(&g, "oC0 = r0;\n");
    /* Outputs to 8-bit targets that the device stores as 32-bit float
       (d3dsh_unorm): the 0..1 range of the unorm target. */
    for (k = 0; k < 4; k++)
        if (unorm->clamp & ps->color_outputs & (1u << k)) emit(&g, "oC%d = d3d_unorm(oC%d);\n", k, k);
    if (ps->color_outputs & 1u)
        emit(&g, "if (ps_misc[0].x != 0.0 && !d3d_alpha(int(ps_misc[0].x), oC0.a, ps_misc[0].y)) discard_fragment();\n");
    /* Blending in the shader (d3dsh_unorm), after the alpha test. */
    for (k = 0; k < 4; k++)
    {
        const char *fs = blend_factor_msl(unorm->src, false), *fd = blend_factor_msl(unorm->dst, false);
        const char *fsa = blend_factor_msl(unorm->src_a, true), *fda = blend_factor_msl(unorm->dst_a, true);
        if (!(unorm->blend & ps->color_outputs & (1u << k))) continue;
        if (!(unorm->clamp & (1u << k))) fail(&g, "blending in the shader of output %d without the unorm clamp", k);
        if (fs == NULL || fd == NULL || fsa == NULL || fda == NULL)
        {
            fail(&g, "blend factors %u, %u, %u, %u are not supported for blending in the shader", unorm->src,
                 unorm->dst, unorm->src_a, unorm->dst_a);
            break;
        }
        {
            char rgb[160], alpha[160];
            if (!blend_op_msl(rgb, sizeof rgb, unorm->op, "s.rgb", fs, "d.rgb", fd) ||
                !blend_op_msl(alpha, sizeof alpha, unorm->op_a, "s.a", fsa, "d.a", fda))
            {
                fail(&g, "blend operations %u, %u are not valid", unorm->op, unorm->op_a);
                break;
            }
            emit(&g, "{\n");
            g.indent++;
            emit(&g, "float4 s = oC%d, d = dst%d;\n", k, k);
            emit(&g, "float3 rgb = %s;\n", rgb);
            emit(&g, "float alpha = %s;\n", alpha);
        }
        emit(&g, "oC%d = d3d_unorm(float4(rgb, alpha));\n", k);
        g.indent--;
        emit(&g, "}\n");
    }
    emit(&g, "POut out;\n");
    for (k = 0; k < 4; k++)
        if (ps->color_outputs & (1u << k)) emit(&g, "out.oC%d = oC%d;\n", k, k);
    if (ps->writes_depth) emit(&g, "out.oDepth = oDepth;\n");
    emit(&g, "return out;\n");
    g.indent = 0;
    emit(&g, "}\n");
    return finish(&g, out);
}

