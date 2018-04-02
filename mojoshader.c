/**
 * MojoShader; generate shader programs from bytecode of compiled
 *  Direct3D shaders.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

// !!! FIXME: this file really needs to be split up.
// !!! FIXME: I keep changing coding styles for symbols and typedefs.

// !!! FIXME: rules from MSDN about temp registers we probably don't check.
// - There are limited temporaries: vs_1_1 has 12 (ps_1_1 has _2_!).
// - SM2 apparently was variable, between 12 and 32. Shader Model 3 has 32.
// - A maximum of three temp registers can be used in a single instruction.

#define __MOJOSHADER_INTERNAL__ 1
#include "mojoshader_internal.h"

typedef struct ConstantsList
{
    MOJOSHADER_constant constant;
    struct ConstantsList *next;
} ConstantsList;

typedef struct VariableList
{
    MOJOSHADER_uniformType type;
    // The indices of the constants that make up this variable are given by
    // the range [index, index + count)
    int index;
    // `count` constants make up this variable
    int count;
    // Pointer to the first of `count` elements in a linked list of constants
    // that make up this variable.
    ConstantsList *constant;
    int used;
    // used in some profiles like GLSL, where `constant == NULL` indicates that `emit_position` is used instead
    int emit_position;
    struct VariableList *next;
} VariableList;

typedef struct RegisterList
{
    RegisterType regtype;
    int regnum;
    MOJOSHADER_usage usage;
    unsigned int index;
    int writemask;
    int misc;
    int written;
#if SUPPORT_PROFILE_SPIRV
    struct {
        uint32 iddecl;
        uint32 iduse;
    } spirv;
#endif
    const VariableList *array;
    struct RegisterList *next;
} RegisterList;

#if SUPPORT_PROFILE_SPIRV
// For baked-in constants in SPIR-V we want to store scalar values that we can
// use in composites, since OpConstantComposite uses result ids constituates
// rather than value literals.
// We'll store these lists grouped by type and have the lists themselves
// ordered by value in the ctx.spirv struct.
typedef struct ComponentList
{
    // result id from OpConstant
    uint32 id;
    union {
        float f;
        int i;
        uint32 u;
    } v;
    struct ComponentList *next;
} ComponentList;
#endif

typedef struct
{
    const uint32 *token;   // this is the unmolested token in the stream.
    int regnum;
    int swizzle;  // xyzw (all four, not split out).
    int swizzle_x;
    int swizzle_y;
    int swizzle_z;
    int swizzle_w;
    SourceMod src_mod;
    RegisterType regtype;
    int relative;
    RegisterType relative_regtype;
    int relative_regnum;
    int relative_component;
    const VariableList *relative_array;
} SourceArgInfo;

struct Profile;  // predeclare.

typedef struct CtabData
{
    int have_ctab;
    int symbol_count;
    MOJOSHADER_symbol *symbols;
} CtabData;

// Context...this is state that changes as we parse through a shader...
typedef struct Context
{
    int isfail;

    int out_of_memory;
    MOJOSHADER_malloc malloc;
    MOJOSHADER_free free;
    void *malloc_data;

    // orig_tokens + current_position == tokens
    int current_position;
    const uint32 *orig_tokens;
    const uint32 *tokens;
    uint32 tokencount; // How many remaining in `tokens`
    // If !know_shader_size then tokencount is always 0xFFFFFFFF (an obscenely large value)
    // and finding an `end` token is what tells us to terminate.
    int know_shader_size;

    const MOJOSHADER_swizzle *swizzles;
    unsigned int swizzles_count;

    const MOJOSHADER_samplerMap *samplermap;
    unsigned int samplermap_count;

    // Buffers for each of the different sections of the generated code
    Buffer *preflight;
    Buffer *globals;
    Buffer *inputs;
    Buffer *outputs;
    Buffer *helpers;
    Buffer *subroutines;
    Buffer *mainline_intro;
    Buffer *mainline_arguments;
    Buffer *mainline_top;
    Buffer *mainline;
    Buffer *postflight;
    Buffer *ignore;

    // `output` points to whichever one of the above buffers is the current output
    Buffer *output;
    Buffer *output_stack[3];
    int output_stack_len;
    int indent_stack[3];
    int indent;

    const char *shader_type_str; // "vs" or "ps"

    // Line terminator for the platform
    const char *endline;
    int endline_len;

    const char *mainfn;
    int profileid;
    const struct Profile *profile;
    MOJOSHADER_shaderType shader_type;
    uint8 major_ver;
    uint8 minor_ver;
    uint32 version_token; // the raw token that contains shader_type, major_ver, and minor_ver

    // Values parsed from the current instruction
    DestArgInfo dest_arg;
    SourceArgInfo source_args[5];
    SourceArgInfo predicate_arg;  // for predicated instructions.
    int predicated;
    uint32 dwords[4]; // For passing miscellaneous constants, such as immediates, register usage, or sampler texture format
    uint32 instruction_controls;
    uint32 previous_opcode;
    int coissue;

    // Is the current instruction allowed to use MOD_CENTROID
    int centroid_allowed;

    int instruction_count;

    int loops;
    int reps;
    int max_reps;
    int cmps;

    int scratch_registers;
    int max_scratch_registers;

    int branch_labels_stack_index;
    int branch_labels_stack[32];

    int assigned_branch_labels;
    int assigned_vertex_attributes;
    int last_address_reg_component;

    // Registers are inserted into this anytime they are read or written. Temporary registers (r#) are asserted
    // to have been written before being read.
    RegisterList used_registers;
    // DEF, DEFI, DEFB, DCL, and label instructions insert their dest args into this list
    RegisterList defined_registers;

    ErrorList *errors;

    int constant_count;
    ConstantsList *constants;

    int uniform_count;
    int uniform_float4_count;
    int uniform_int4_count;
    int uniform_bool_count;
    RegisterList uniforms;

    int attribute_count;
    RegisterList attributes;

    int sampler_count;
    RegisterList samplers;

    VariableList *variables;  // variables to register mapping.
    CtabData ctab;

    // Have we seen any instructions that relative-address any input attributes
    int have_relative_input_registers;
    // Have we used more than one register of type REG_TYPE_COLOROUT
    int have_multi_color_outputs;

    // Records whether `determine_constant_arrays()` has been called, which means that all DEF*
    // instruction should now have been seen, the `ctx->constants` list is now sorted by the
    // index of each constant, and the `ctx->variables` list has been built, which records the
    // constant[s] that make up each variable.
    int determined_constants_arrays;

    // Is there any output attribute register defined with usage == MOJOSHADER_USAGE_POINTSIZE
    int uses_pointsize;

    // Is there any output attribute register defined with usage == MOJOSHADER_USAGE_FOG
    int uses_fog;

    // !!! FIXME: move these into SUPPORT_PROFILE sections.
    int glsl_generated_lit_helper;
    int glsl_generated_texldd_setup;
    int glsl_generated_texm3x3spec_helper;
    int arb1_wrote_position;
    // !!! FIXME: move these into SUPPORT_PROFILE sections.

    int have_preshader;
    int ignores_ctab;
    int reset_texmpad;
    int texm3x2pad_dst0;
    int texm3x2pad_src0;
    int texm3x3pad_dst0;
    int texm3x3pad_src0;
    int texm3x3pad_dst1;
    int texm3x3pad_src1;
    MOJOSHADER_preshader *preshader;

#if SUPPORT_PROFILE_ARB1_NV
    int profile_supports_nv2;
    int profile_supports_nv3;
    int profile_supports_nv4;
#endif
#if SUPPORT_PROFILE_GLSL120
    int profile_supports_glsl120;
#endif
#if SUPPORT_PROFILE_GLSLES
    int profile_supports_glsles;
#endif

#if SUPPORT_PROFILE_METAL
    int metal_need_header_common;
    int metal_need_header_math;
    int metal_need_header_relational;
    int metal_need_header_geometric;
    int metal_need_header_graphics;
    int metal_need_header_texture;
#endif

#if SUPPORT_PROFILE_SPIRV
    struct {
        // ext. glsl instructions have been imported
        uint32 idext;
        uint32 idmax;
        uint32 idmain;
        uint32 inoutcount;
        // ids for types so we can reuse them after they're declared
        struct {
            uint32 idvoid;
            uint32 idfuncv;
            uint32 idbool;
            uint32 idtrue;
            uint32 idfalse;

            uint32 idfloat;
            uint32 idint;
            uint32 iduint;
            uint32 idvec4;
            uint32 idivec4;
            uint32 idvec3;

            uint32 idimage2d;
            uint32 idimage3d;
            uint32 idimagecube;

            uint32 idptrvec4u;
            uint32 idptrivec4u;
            uint32 idptrvec4i;
            uint32 idptrivec4i;
            uint32 idptrvec4o;
            uint32 idptrivec4o;
            uint32 idptrvec4p;
            uint32 idptrivec4p;
            uint32 idptrfloato;

            uint32 idptrimage2d;
            uint32 idptrimage3d;
            uint32 idptrimagecube;
        } types;
        struct {
            ComponentList f;
            ComponentList i;
            ComponentList u;
        } cl;
    } spirv;
#endif
} Context;


// Use these macros so we can remove all bits of these profiles from the build.
#if SUPPORT_PROFILE_ARB1_NV
#define support_nv2(ctx) ((ctx)->profile_supports_nv2)
#define support_nv3(ctx) ((ctx)->profile_supports_nv3)
#define support_nv4(ctx) ((ctx)->profile_supports_nv4)
#else
#define support_nv2(ctx) (0)
#define support_nv3(ctx) (0)
#define support_nv4(ctx) (0)
#endif

#if SUPPORT_PROFILE_GLSL120
#define support_glsl120(ctx) ((ctx)->profile_supports_glsl120)
#else
#define support_glsl120(ctx) (0)
#endif

#if SUPPORT_PROFILE_GLSLES
#define support_glsles(ctx) ((ctx)->profile_supports_glsles)
#else
#define support_glsles(ctx) (0)
#endif


// Profile entry points...

// one emit function for each opcode in each profile.
typedef void (*emit_function)(Context *ctx);

// one emit function for starting output in each profile.
typedef void (*emit_start)(Context *ctx, const char *profilestr);

// one emit function for ending output in each profile.
typedef void (*emit_end)(Context *ctx);

// one emit function for phase opcode output in each profile.
typedef void (*emit_phase)(Context *ctx);

// one emit function for finalizing output in each profile.
typedef void (*emit_finalize)(Context *ctx);

// one emit function for global definitions in each profile.
typedef void (*emit_global)(Context *ctx, RegisterType regtype, int regnum);

// one emit function for relative uniform arrays in each profile.
typedef void (*emit_array)(Context *ctx, VariableList *var);

// one emit function for relative constants arrays in each profile.
typedef void (*emit_const_array)(Context *ctx,
                                 const struct ConstantsList *constslist,
                                 int base, int size);

// one emit function for uniforms in each profile.
typedef void (*emit_uniform)(Context *ctx, RegisterType regtype, int regnum,
                             const VariableList *var);

// one emit function for samplers in each profile.
typedef void (*emit_sampler)(Context *ctx, int stage, TextureType ttype,
                             int texbem);

// one emit function for attributes in each profile.
typedef void (*emit_attribute)(Context *ctx, RegisterType regtype, int regnum,
                               MOJOSHADER_usage usage, int index, int wmask,
                               int flags);

// one args function for each possible sequence of opcode arguments.
typedef int (*args_function)(Context *ctx);

// one state function for each opcode where we have state machine updates.
typedef void (*state_function)(Context *ctx);

// one function for varnames in each profile.
typedef const char *(*varname_function)(Context *c, RegisterType t, int num);

// one function for const var array in each profile.
typedef const char *(*const_array_varname_function)(Context *c, int base, int size);

typedef struct Profile
{
    const char *name;
    emit_start start_emitter;
    emit_end end_emitter;
    emit_phase phase_emitter;
    emit_global global_emitter;
    emit_array array_emitter;
    emit_const_array const_array_emitter;
    emit_uniform uniform_emitter;
    emit_sampler sampler_emitter;
    emit_attribute attribute_emitter;
    emit_finalize finalize_emitter;
    varname_function get_varname;
    const_array_varname_function get_const_array_varname;
} Profile;


// !!! FIXME: cut and paste between every damned source file follows...
// !!! FIXME: We need to make some sort of ContextBase that applies to all
// !!! FIXME:  files and move this stuff to mojoshader_common.c ...

static inline void out_of_memory(Context *ctx)
{
    ctx->isfail = ctx->out_of_memory = 1;
} // out_of_memory

static inline void *Malloc(Context *ctx, const size_t len)
{
    void *retval = ctx->malloc((int) len, ctx->malloc_data);
    if (retval == NULL)
        out_of_memory(ctx);
    return retval;
} // Malloc

static inline char *StrDup(Context *ctx, const char *str)
{
    char *retval = (char *) Malloc(ctx, strlen(str) + 1);
    if (retval != NULL)
        strcpy(retval, str);
    return retval;
} // StrDup

static inline void Free(Context *ctx, void *ptr)
{
    ctx->free(ptr, ctx->malloc_data);
} // Free

static void * MOJOSHADERCALL MallocBridge(int bytes, void *data)
{
    return Malloc((Context *) data, (size_t) bytes);
} // MallocBridge

static void MOJOSHADERCALL FreeBridge(void *ptr, void *data)
{
    Free((Context *) data, ptr);
} // FreeBridge


// jump between output sections in the context...

static int set_output(Context *ctx, Buffer **section)
{
    // only create output sections on first use.
    if (*section == NULL)
    {
        *section = buffer_create(256, MallocBridge, FreeBridge, ctx);
        if (*section == NULL)
            return 0;
    } // if

    ctx->output = *section;
    return 1;
} // set_output

static void push_output(Context *ctx, Buffer **section)
{
    assert(ctx->output_stack_len < (int) (STATICARRAYLEN(ctx->output_stack)));
    ctx->output_stack[ctx->output_stack_len] = ctx->output;
    ctx->indent_stack[ctx->output_stack_len] = ctx->indent;
    ctx->output_stack_len++;
    if (!set_output(ctx, section))
        return;
    ctx->indent = 0;
} // push_output

static inline void pop_output(Context *ctx)
{
    assert(ctx->output_stack_len > 0);
    ctx->output_stack_len--;
    ctx->output = ctx->output_stack[ctx->output_stack_len];
    ctx->indent = ctx->indent_stack[ctx->output_stack_len];
} // pop_output



// Shader model version magic...

static inline uint32 ver_ui32(const uint8 major, const uint8 minor)
{
    return ( (((uint32) major) << 16) | (((minor) == 0xFF) ? 1 : (minor)) );
} // version_ui32

static inline int shader_version_supported(const uint8 maj, const uint8 min)
{
    return (ver_ui32(maj,min) <= ver_ui32(MAX_SHADER_MAJOR, MAX_SHADER_MINOR));
} // shader_version_supported

static inline int shader_version_atleast(const Context *ctx, const uint8 maj,
                                         const uint8 min)
{
    return (ver_ui32(ctx->major_ver, ctx->minor_ver) >= ver_ui32(maj, min));
} // shader_version_atleast

static inline int shader_version_exactly(const Context *ctx, const uint8 maj,
                                         const uint8 min)
{
    return ((ctx->major_ver == maj) && (ctx->minor_ver == min));
} // shader_version_exactly

static inline int shader_is_pixel(const Context *ctx)
{
    return (ctx->shader_type == MOJOSHADER_TYPE_PIXEL);
} // shader_is_pixel

static inline int shader_is_vertex(const Context *ctx)
{
    return (ctx->shader_type == MOJOSHADER_TYPE_VERTEX);
} // shader_is_vertex


static inline int isfail(const Context *ctx)
{
    return ctx->isfail;
} // isfail


static void failf(Context *ctx, const char *fmt, ...) ISPRINTF(2,3);
static void failf(Context *ctx, const char *fmt, ...)
{
    ctx->isfail = 1;
    if (ctx->out_of_memory)
        return;

    // no filename at this level (we pass a NULL to errorlist_add_va()...)
    va_list ap;
    va_start(ap, fmt);
    errorlist_add_va(ctx->errors, NULL, ctx->current_position, fmt, ap);
    va_end(ap);
} // failf


static inline void fail(Context *ctx, const char *reason)
{
    failf(ctx, "%s", reason);
} // fail


static void output_line(Context *ctx, const char *fmt, ...) ISPRINTF(2,3);
static void output_line(Context *ctx, const char *fmt, ...)
{
    assert(ctx->output != NULL);
    if (isfail(ctx))
        return;  // we failed previously, don't go on...

    const int indent = ctx->indent;
    if (indent > 0)
    {
        char *indentbuf = (char *) alloca(indent);
        memset(indentbuf, '\t', indent);
        buffer_append(ctx->output, indentbuf, indent);
    } // if

    va_list ap;
    va_start(ap, fmt);
    buffer_append_va(ctx->output, fmt, ap);
    va_end(ap);

    buffer_append(ctx->output, ctx->endline, ctx->endline_len);
} // output_line


static inline void output_blank_line(Context *ctx)
{
    assert(ctx->output != NULL);
    if (!isfail(ctx))
        buffer_append(ctx->output, ctx->endline, ctx->endline_len);
} // output_blank_line


// !!! FIXME: this is sort of nasty.
static void floatstr(Context *ctx, char *buf, size_t bufsize, float f,
                     int leavedecimal)
{
    const size_t len = MOJOSHADER_printFloat(buf, bufsize, f);
    if ((len+2) >= bufsize)
        fail(ctx, "BUG: internal buffer is too small");
    else
    {
        char *end = buf + len;
        char *ptr = strchr(buf, '.');
        if (ptr == NULL)
        {
            if (leavedecimal)
                strcat(buf, ".0");
            return;  // done.
        } // if

        while (--end != ptr)
        {
            if (*end != '0')
            {
                end++;
                break;
            } // if
        } // while
        if ((leavedecimal) && (end == ptr))
            end += 2;
        *end = '\0';  // chop extra '0' or all decimal places off.
    } // else
} // floatstr

static inline TextureType cvtMojoToD3DSamplerType(const MOJOSHADER_samplerType type)
{
    return (TextureType) (((int) type) + 2);
} // cvtMojoToD3DSamplerType

static inline MOJOSHADER_samplerType cvtD3DToMojoSamplerType(const TextureType type)
{
    return (MOJOSHADER_samplerType) (((int) type) - 2);
} // cvtD3DToMojoSamplerType


// Deal with register lists...  !!! FIXME: I sort of hate this.

static void free_reglist(MOJOSHADER_free f, void *d, RegisterList *item)
{
    while (item != NULL)
    {
        RegisterList *next = item->next;
        f(item, d);
        item = next;
    } // while
} // free_reglist

static inline uint32 reg_to_ui32(const RegisterType regtype, const int regnum)
{
    return ( ((uint32) regnum) | (((uint32) regtype) << 16) );
} // reg_to_uint32

// !!! FIXME: ditch this for a hash table.
static RegisterList *reglist_insert(Context *ctx, RegisterList *prev,
                                    const RegisterType regtype,
                                    const int regnum)
{
    const uint32 newval = reg_to_ui32(regtype, regnum);
    RegisterList *item = prev->next;
    while (item != NULL)
    {
        const uint32 val = reg_to_ui32(item->regtype, item->regnum);
        if (newval == val)
            return item;  // already set, so we're done.
        else if (newval < val)  // insert it here.
            break;
        else // if (newval > val)
        {
            // keep going, we're not to the insertion point yet.
            prev = item;
            item = item->next;
        } // else
    } // while

    // we need to insert an entry after (prev).
    item = (RegisterList *) Malloc(ctx, sizeof (RegisterList));
    if (item != NULL)
    {
        item->regtype = regtype;
        item->regnum = regnum;
        item->usage = MOJOSHADER_USAGE_UNKNOWN;
        item->index = 0;
        item->writemask = 0;
        item->misc = 0;
        item->written = 0;
#if SUPPORT_PROFILE_SPIRV
        item->spirv.iddecl = 0;
        item->spirv.iduse = 0;
#endif
        item->array = NULL;
        item->next = prev->next;
        prev->next = item;
    } // if

    return item;
} // reglist_insert

static RegisterList *reglist_find(const RegisterList *prev,
                                  const RegisterType rtype, const int regnum)
{
    const uint32 newval = reg_to_ui32(rtype, regnum);
    RegisterList *item = prev->next;
    while (item != NULL)
    {
        const uint32 val = reg_to_ui32(item->regtype, item->regnum);
        if (newval == val)
            return item;  // here it is.
        else if (newval < val)  // should have been here if it existed.
            return NULL;
        else // if (newval > val)
            item = item->next;
    } // while

    return NULL;  // wasn't in the list.
} // reglist_find

static inline const RegisterList *reglist_exists(RegisterList *prev,
                                                 const RegisterType regtype,
                                                 const int regnum)
{
    return (reglist_find(prev, regtype, regnum));
} // reglist_exists

static inline int register_was_written(Context *ctx, const RegisterType rtype,
                                       const int regnum)
{
    RegisterList *reg = reglist_find(&ctx->used_registers, rtype, regnum);
    return (reg && reg->written);
} // register_was_written

static inline RegisterList *set_used_register(Context *ctx,
                                              const RegisterType regtype,
                                              const int regnum,
                                              const int written)
{
    RegisterList *reg = NULL;
    if ((regtype == REG_TYPE_COLOROUT) && (regnum > 0))
        ctx->have_multi_color_outputs = 1;

    reg = reglist_insert(ctx, &ctx->used_registers, regtype, regnum);
    if (reg && written)
        reg->written = 1;
    return reg;
} // set_used_register

static inline int get_used_register(Context *ctx, const RegisterType regtype,
                                    const int regnum)
{
    return (reglist_exists(&ctx->used_registers, regtype, regnum) != NULL);
} // get_used_register

static inline void set_defined_register(Context *ctx, const RegisterType rtype,
                                        const int regnum)
{
    reglist_insert(ctx, &ctx->defined_registers, rtype, regnum);
} // set_defined_register

static inline int get_defined_register(Context *ctx, const RegisterType rtype,
                                       const int regnum)
{
    return (reglist_exists(&ctx->defined_registers, rtype, regnum) != NULL);
} // get_defined_register

static void add_attribute_register(Context *ctx, const RegisterType rtype,
                                const int regnum, const MOJOSHADER_usage usage,
                                const int index, const int writemask, int flags)
{
    RegisterList *item = reglist_insert(ctx, &ctx->attributes, rtype, regnum);
    item->usage = usage;
    item->index = index;
    item->writemask = writemask;
    item->misc = flags;

    if ((rtype == REG_TYPE_OUTPUT) && (usage == MOJOSHADER_USAGE_POINTSIZE))
        ctx->uses_pointsize = 1;  // note that we have to check this later.
    else if ((rtype == REG_TYPE_OUTPUT) && (usage == MOJOSHADER_USAGE_FOG))
        ctx->uses_fog = 1;  // note that we have to check this later.
} // add_attribute_register

static inline void add_sampler(Context *ctx, const int regnum,
                               TextureType ttype, const int texbem)
{
    const RegisterType rtype = REG_TYPE_SAMPLER;

    // !!! FIXME: make sure it doesn't exist?
    // !!! FIXME:  (ps_1_1 assume we can add it multiple times...)
    RegisterList *item = reglist_insert(ctx, &ctx->samplers, rtype, regnum);

    if (ctx->samplermap != NULL)
    {
        unsigned int i;
        for (i = 0; i < ctx->samplermap_count; i++)
        {
            if (ctx->samplermap[i].index == regnum)
            {
                ttype = cvtMojoToD3DSamplerType(ctx->samplermap[i].type);
                break;
            } // if
        } // for
    } // if

    item->index = (int) ttype;
    item->misc |= texbem;
} // add_sampler


static inline int writemask_xyzw(const int writemask)
{
    return (writemask == 0xF);  // 0xF == 1111. No explicit mask (full!).
} // writemask_xyzw


static inline int writemask_xyz(const int writemask)
{
    return (writemask == 0x7);  // 0x7 == 0111. (that is: xyz)
} // writemask_xyz


static inline int writemask_xy(const int writemask)
{
    return (writemask == 0x3);  // 0x3 == 0011. (that is: xy)
} // writemask_xy


static inline int writemask_x(const int writemask)
{
    return (writemask == 0x1);  // 0x1 == 0001. (that is: x)
} // writemask_x


static inline int writemask_y(const int writemask)
{
    return (writemask == 0x2);  // 0x1 == 0010. (that is: y)
} // writemask_y


static inline int replicate_swizzle(const int swizzle)
{
    return ( (((swizzle >> 0) & 0x3) == ((swizzle >> 2) & 0x3)) &&
             (((swizzle >> 2) & 0x3) == ((swizzle >> 4) & 0x3)) &&
             (((swizzle >> 4) & 0x3) == ((swizzle >> 6) & 0x3)) );
} // replicate_swizzle


static inline int no_swizzle(const int swizzle)
{
    return (swizzle == 0xE4);  // 0xE4 == 11100100 ... 0 1 2 3. No swizzle.
} // no_swizzle


static inline int vecsize_from_writemask(const int m)
{
    return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1);
} // vecsize_from_writemask


static inline void set_dstarg_writemask(DestArgInfo *dst, const int mask)
{
    dst->writemask = mask;
    dst->writemask0 = ((mask >> 0) & 1);
    dst->writemask1 = ((mask >> 1) & 1);
    dst->writemask2 = ((mask >> 2) & 1);
    dst->writemask3 = ((mask >> 3) & 1);
} // set_dstarg_writemask


static int allocate_scratch_register(Context *ctx)
{
    const int retval = ctx->scratch_registers++;
    if (retval >= ctx->max_scratch_registers)
        ctx->max_scratch_registers = retval + 1;
    return retval;
} // allocate_scratch_register

static int allocate_branch_label(Context *ctx)
{
    return ctx->assigned_branch_labels++;
} // allocate_branch_label

static inline void adjust_token_position(Context *ctx, const int incr)
{
    ctx->tokens += incr;
    ctx->tokencount -= incr;
    ctx->current_position += incr * sizeof (uint32);
} // adjust_token_position


// D3D stuff that's used in more than just the d3d profile...

static int isscalar(Context *ctx, const MOJOSHADER_shaderType shader_type,
                    const RegisterType rtype, const int rnum)
{
    const int uses_psize = ctx->uses_pointsize;
    const int uses_fog = ctx->uses_fog;
    if ( (rtype == REG_TYPE_OUTPUT) && ((uses_psize) || (uses_fog)) )
    {
        const RegisterList *reg = reglist_find(&ctx->attributes, rtype, rnum);
        if (reg != NULL)
        {
            const MOJOSHADER_usage usage = reg->usage;
            return ( (uses_psize && (usage == MOJOSHADER_USAGE_POINTSIZE)) ||
                     (uses_fog && (usage == MOJOSHADER_USAGE_FOG)) );
        } // if
    } // if

    return scalar_register(shader_type, rtype, rnum);
} // isscalar

static const char swizzle_channels[] = { 'x', 'y', 'z', 'w' };


static const char *usagestrs[] = {
    "_position", "_blendweight", "_blendindices", "_normal", "_psize",
    "_texcoord", "_tangent", "_binormal", "_tessfactor", "_positiont",
    "_color", "_fog", "_depth", "_sample"
};

static const char *get_D3D_register_string(Context *ctx,
                                           RegisterType regtype,
                                           int regnum, char *regnum_str,
                                           size_t regnum_size)
{
    const char *retval = NULL;
    int has_number = 1;

    switch (regtype)
    {
        case REG_TYPE_TEMP:
            retval = "r";
            break;

        case REG_TYPE_INPUT:
            retval = "v";
            break;

        case REG_TYPE_CONST:
            retval = "c";
            break;

        case REG_TYPE_ADDRESS:  // (or REG_TYPE_TEXTURE, same value.)
            retval = shader_is_vertex(ctx) ? "a" : "t";
            break;

        case REG_TYPE_RASTOUT:
            switch ((RastOutType) regnum)
            {
                case RASTOUT_TYPE_POSITION: retval = "oPos"; break;
                case RASTOUT_TYPE_FOG: retval = "oFog"; break;
                case RASTOUT_TYPE_POINT_SIZE: retval = "oPts"; break;
            } // switch
            has_number = 0;
            break;

        case REG_TYPE_ATTROUT:
            retval = "oD";
            break;

        case REG_TYPE_OUTPUT: // (or REG_TYPE_TEXCRDOUT, same value.)
            if (shader_is_vertex(ctx) && shader_version_atleast(ctx, 3, 0))
                retval = "o";
            else
                retval = "oT";
            break;

        case REG_TYPE_CONSTINT:
            retval = "i";
            break;

        case REG_TYPE_COLOROUT:
            retval = "oC";
            break;

        case REG_TYPE_DEPTHOUT:
            retval = "oDepth";
            has_number = 0;
            break;

        case REG_TYPE_SAMPLER:
            retval = "s";
            break;

        case REG_TYPE_CONSTBOOL:
            retval = "b";
            break;

        case REG_TYPE_LOOP:
            retval = "aL";
            has_number = 0;
            break;

        case REG_TYPE_MISCTYPE:
            switch ((const MiscTypeType) regnum)
            {
                case MISCTYPE_TYPE_POSITION: retval = "vPos"; break;
                case MISCTYPE_TYPE_FACE: retval = "vFace"; break;
            } // switch
            has_number = 0;
            break;

        case REG_TYPE_LABEL:
            retval = "l";
            break;

        case REG_TYPE_PREDICATE:
            retval = "p";
            break;

        //case REG_TYPE_TEMPFLOAT16:  // !!! FIXME: don't know this asm string
        default:
            fail(ctx, "unknown register type");
            retval = "???";
            has_number = 0;
            break;
    } // switch

    if (has_number)
        snprintf(regnum_str, regnum_size, "%u", (uint) regnum);
    else
        regnum_str[0] = '\0';

    return retval;
} // get_D3D_register_string


// !!! FIXME: can we split the profile code out to separate source files?

#define AT_LEAST_ONE_PROFILE 0

#if !SUPPORT_PROFILE_D3D
#define PROFILE_EMITTER_D3D(op)
#else
#undef AT_LEAST_ONE_PROFILE
#define AT_LEAST_ONE_PROFILE 1
#define PROFILE_EMITTER_D3D(op) emit_D3D_##op,

static const char *make_D3D_srcarg_string_in_buf(Context *ctx,
                                                 const SourceArgInfo *arg,
                                                 char *buf, size_t buflen)
{
    const char *premod_str = "";
    const char *postmod_str = "";
    switch (arg->src_mod)
    {
        case SRCMOD_NEGATE:
            premod_str = "-";
            break;

        case SRCMOD_BIASNEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_BIAS:
            postmod_str = "_bias";
            break;

        case SRCMOD_SIGNNEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_SIGN:
            postmod_str = "_bx2";
            break;

        case SRCMOD_COMPLEMENT:
            premod_str = "1-";
            break;

        case SRCMOD_X2NEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_X2:
            postmod_str = "_x2";
            break;

        case SRCMOD_DZ:
            postmod_str = "_dz";
            break;

        case SRCMOD_DW:
            postmod_str = "_dw";
            break;

        case SRCMOD_ABSNEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_ABS:
            postmod_str = "_abs";
            break;

        case SRCMOD_NOT:
            premod_str = "!";
            break;

        case SRCMOD_NONE:
        case SRCMOD_TOTAL:
             break;  // stop compiler whining.
    } // switch


    char regnum_str[16];
    const char *regtype_str = get_D3D_register_string(ctx, arg->regtype,
                                                      arg->regnum, regnum_str,
                                                      sizeof (regnum_str));

    if (regtype_str == NULL)
    {
        fail(ctx, "Unknown source register type.");
        *buf = '\0';
        return buf;
    } // if

    const char *rel_lbracket = "";
    const char *rel_rbracket = "";
    char rel_swizzle[4] = { '\0' };
    char rel_regnum_str[16] = { '\0' };
    const char *rel_regtype_str = "";
    if (arg->relative)
    {
        rel_swizzle[0] = '.';
        rel_swizzle[1] = swizzle_channels[arg->relative_component];
        rel_swizzle[2] = '\0';
        rel_lbracket = "[";
        rel_rbracket = "]";
        rel_regtype_str = get_D3D_register_string(ctx, arg->relative_regtype,
                                                  arg->relative_regnum,
                                                  rel_regnum_str,
                                                  sizeof (rel_regnum_str));

        if (regtype_str == NULL)
        {
            fail(ctx, "Unknown relative source register type.");
            *buf = '\0';
            return buf;
        } // if
    } // if

    char swizzle_str[6];
    size_t i = 0;
    const int scalar = isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum);
    if (!scalar && !no_swizzle(arg->swizzle))
    {
        swizzle_str[i++] = '.';
        swizzle_str[i++] = swizzle_channels[arg->swizzle_x];
        swizzle_str[i++] = swizzle_channels[arg->swizzle_y];
        swizzle_str[i++] = swizzle_channels[arg->swizzle_z];
        swizzle_str[i++] = swizzle_channels[arg->swizzle_w];

        // .xyzz is the same as .xyz, .z is the same as .zzzz, etc.
        while (swizzle_str[i-1] == swizzle_str[i-2])
            i--;
    } // if
    swizzle_str[i] = '\0';
    assert(i < sizeof (swizzle_str));

    // !!! FIXME: c12[a0.x] actually needs to be c[a0.x + 12]
    snprintf(buf, buflen, "%s%s%s%s%s%s%s%s%s%s",
             premod_str, regtype_str, regnum_str, postmod_str,
             rel_lbracket, rel_regtype_str, rel_regnum_str, rel_swizzle,
             rel_rbracket, swizzle_str);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_D3D_srcarg_string_in_buf


static const char *make_D3D_destarg_string(Context *ctx, char *buf,
                                           const size_t buflen)
{
    const DestArgInfo *arg = &ctx->dest_arg;

    const char *result_shift_str = "";
    switch (arg->result_shift)
    {
        case 0x1: result_shift_str = "_x2"; break;
        case 0x2: result_shift_str = "_x4"; break;
        case 0x3: result_shift_str = "_x8"; break;
        case 0xD: result_shift_str = "_d8"; break;
        case 0xE: result_shift_str = "_d4"; break;
        case 0xF: result_shift_str = "_d2"; break;
    } // switch

    const char *sat_str = (arg->result_mod & MOD_SATURATE) ? "_sat" : "";
    const char *pp_str = (arg->result_mod & MOD_PP) ? "_pp" : "";
    const char *cent_str = (arg->result_mod & MOD_CENTROID) ? "_centroid" : "";

    char regnum_str[16];
    const char *regtype_str = get_D3D_register_string(ctx, arg->regtype,
                                                      arg->regnum, regnum_str,
                                                      sizeof (regnum_str));
    if (regtype_str == NULL)
    {
        fail(ctx, "Unknown destination register type.");
        *buf = '\0';
        return buf;
    } // if

    char writemask_str[6];
    size_t i = 0;
    const int scalar = isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum);
    if (!scalar && !writemask_xyzw(arg->writemask))
    {
        writemask_str[i++] = '.';
        if (arg->writemask0) writemask_str[i++] = 'x';
        if (arg->writemask1) writemask_str[i++] = 'y';
        if (arg->writemask2) writemask_str[i++] = 'z';
        if (arg->writemask3) writemask_str[i++] = 'w';
    } // if
    writemask_str[i] = '\0';
    assert(i < sizeof (writemask_str));

    const char *pred_left = "";
    const char *pred_right = "";
    char pred[32] = { '\0' };
    if (ctx->predicated)
    {
        pred_left = "(";
        pred_right = ") ";
        make_D3D_srcarg_string_in_buf(ctx, &ctx->predicate_arg,
                                      pred, sizeof (pred));
    } // if

    // may turn out something like "_x2_sat_pp_centroid (!p0.x) r0.xyzw" ...
    snprintf(buf, buflen, "%s%s%s%s %s%s%s%s%s%s",
             result_shift_str, sat_str, pp_str, cent_str,
             pred_left, pred, pred_right,
             regtype_str, regnum_str, writemask_str);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_D3D_destarg_string


static const char *make_D3D_srcarg_string(Context *ctx, const size_t idx,
                                          char *buf, size_t buflen)
{
    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        *buf = '\0';
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];
    return make_D3D_srcarg_string_in_buf(ctx, arg, buf, buflen);
} // make_D3D_srcarg_string

static const char *get_D3D_varname_in_buf(Context *ctx, RegisterType rt,
                                           int regnum, char *buf,
                                           const size_t len)
{
    char regnum_str[16];
    const char *regtype_str = get_D3D_register_string(ctx, rt, regnum,
                                              regnum_str, sizeof (regnum_str));
    snprintf(buf,len,"%s%s", regtype_str, regnum_str);
    return buf;
} // get_D3D_varname_in_buf


static const char *get_D3D_varname(Context *ctx, RegisterType rt, int regnum)
{
    char buf[64];
    get_D3D_varname_in_buf(ctx, rt, regnum, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_D3D_varname


static const char *get_D3D_const_array_varname(Context *ctx, int base, int size)
{
    char buf[64];
    snprintf(buf, sizeof (buf), "c_array_%d_%d", base, size);
    return StrDup(ctx, buf);
} // get_D3D_const_array_varname


static void emit_D3D_start(Context *ctx, const char *profilestr)
{
    const uint major = (uint) ctx->major_ver;
    const uint minor = (uint) ctx->minor_ver;
    char minor_str[16];

    ctx->ignores_ctab = 1;

    if (minor == 0xFF)
        strcpy(minor_str, "sw");
    else if ((major > 1) && (minor == 1))
        strcpy(minor_str, "x");  // for >= SM2, apparently this is "x". Weird.
    else
        snprintf(minor_str, sizeof (minor_str), "%u", (uint) minor);

    output_line(ctx, "%s_%u_%s", ctx->shader_type_str, major, minor_str);
} // emit_D3D_start


static void emit_D3D_end(Context *ctx)
{
    output_line(ctx, "end");
} // emit_D3D_end


static void emit_D3D_phase(Context *ctx)
{
    output_line(ctx, "phase");
} // emit_D3D_phase


static void emit_D3D_finalize(Context *ctx)
{
    // no-op.
} // emit_D3D_finalize


static void emit_D3D_global(Context *ctx, RegisterType regtype, int regnum)
{
    // no-op.
} // emit_D3D_global


static void emit_D3D_array(Context *ctx, VariableList *var)
{
    // no-op.
} // emit_D3D_array


static void emit_D3D_const_array(Context *ctx, const ConstantsList *clist,
                                 int base, int size)
{
    // no-op.
} // emit_D3D_const_array


static void emit_D3D_uniform(Context *ctx, RegisterType regtype, int regnum,
                             const VariableList *var)
{
    // no-op.
} // emit_D3D_uniform


static void emit_D3D_sampler(Context *ctx, int s, TextureType ttype, int tb)
{
    // no-op.
} // emit_D3D_sampler


static void emit_D3D_attribute(Context *ctx, RegisterType regtype, int regnum,
                               MOJOSHADER_usage usage, int index, int wmask,
                               int flags)
{
    // no-op.
} // emit_D3D_attribute


static void emit_D3D_RESERVED(Context *ctx)
{
    // do nothing; fails in the state machine.
} // emit_D3D_RESERVED


// Generic D3D opcode emitters. A list of macros generate all the entry points
//  that call into these...

static char *lowercase(char *dst, const char *src)
{
    int i = 0;
    do
    {
        const char ch = src[i];
        dst[i] = (((ch >= 'A') && (ch <= 'Z')) ? (ch - ('A' - 'a')) : ch);
    } while (src[i++]);
    return dst;
} // lowercase


static void emit_D3D_opcode_d(Context *ctx, const char *opcode)
{
    char dst[64]; make_D3D_destarg_string(ctx, dst, sizeof (dst));
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx, "%s%s%s", ctx->coissue ? "+" : "", opcode, dst);
} // emit_D3D_opcode_d


static void emit_D3D_opcode_s(Context *ctx, const char *opcode)
{
    char src0[64]; make_D3D_srcarg_string(ctx, 0, src0, sizeof (src0));
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx, "%s%s %s", ctx->coissue ? "+" : "", opcode, src0);
} // emit_D3D_opcode_s


static void emit_D3D_opcode_ss(Context *ctx, const char *opcode)
{
    char src0[64]; make_D3D_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_D3D_srcarg_string(ctx, 1, src1, sizeof (src1));
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx, "%s%s %s, %s", ctx->coissue ? "+" : "", opcode, src0, src1);
} // emit_D3D_opcode_ss


static void emit_D3D_opcode_ds(Context *ctx, const char *opcode)
{
    char dst[64]; make_D3D_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_D3D_srcarg_string(ctx, 0, src0, sizeof (src0));
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx, "%s%s%s, %s", ctx->coissue ? "+" : "", opcode, dst, src0);
} // emit_D3D_opcode_ds


static void emit_D3D_opcode_dss(Context *ctx, const char *opcode)
{
    char dst[64]; make_D3D_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_D3D_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_D3D_srcarg_string(ctx, 1, src1, sizeof (src1));
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx, "%s%s%s, %s, %s", ctx->coissue ? "+" : "",
                opcode, dst, src0, src1);
} // emit_D3D_opcode_dss


static void emit_D3D_opcode_dsss(Context *ctx, const char *opcode)
{
    char dst[64]; make_D3D_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_D3D_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_D3D_srcarg_string(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_D3D_srcarg_string(ctx, 2, src2, sizeof (src2));
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx, "%s%s%s, %s, %s, %s", ctx->coissue ? "+" : "", 
                opcode, dst, src0, src1, src2);
} // emit_D3D_opcode_dsss


static void emit_D3D_opcode_dssss(Context *ctx, const char *opcode)
{
    char dst[64]; make_D3D_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_D3D_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_D3D_srcarg_string(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_D3D_srcarg_string(ctx, 2, src2, sizeof (src2));
    char src3[64]; make_D3D_srcarg_string(ctx, 3, src3, sizeof (src3));
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx,"%s%s%s, %s, %s, %s, %s", ctx->coissue ? "+" : "",
                opcode, dst, src0, src1, src2, src3);
} // emit_D3D_opcode_dssss


static void emit_D3D_opcode(Context *ctx, const char *opcode)
{
    opcode = lowercase((char *) alloca(strlen(opcode) + 1), opcode);
    output_line(ctx, "%s%s", ctx->coissue ? "+" : "", opcode);
} // emit_D3D_opcode


#define EMIT_D3D_OPCODE_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode(ctx, #op); \
    }
#define EMIT_D3D_OPCODE_D_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode_d(ctx, #op); \
    }
#define EMIT_D3D_OPCODE_S_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode_s(ctx, #op); \
    }
#define EMIT_D3D_OPCODE_SS_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode_ss(ctx, #op); \
    }
#define EMIT_D3D_OPCODE_DS_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode_ds(ctx, #op); \
    }
#define EMIT_D3D_OPCODE_DSS_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode_dss(ctx, #op); \
    }
#define EMIT_D3D_OPCODE_DSSS_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode_dsss(ctx, #op); \
    }
#define EMIT_D3D_OPCODE_DSSSS_FUNC(op) \
    static void emit_D3D_##op(Context *ctx) { \
        emit_D3D_opcode_dssss(ctx, #op); \
    }

EMIT_D3D_OPCODE_FUNC(NOP)
EMIT_D3D_OPCODE_DS_FUNC(MOV)
EMIT_D3D_OPCODE_DSS_FUNC(ADD)
EMIT_D3D_OPCODE_DSS_FUNC(SUB)
EMIT_D3D_OPCODE_DSSS_FUNC(MAD)
EMIT_D3D_OPCODE_DSS_FUNC(MUL)
EMIT_D3D_OPCODE_DS_FUNC(RCP)
EMIT_D3D_OPCODE_DS_FUNC(RSQ)
EMIT_D3D_OPCODE_DSS_FUNC(DP3)
EMIT_D3D_OPCODE_DSS_FUNC(DP4)
EMIT_D3D_OPCODE_DSS_FUNC(MIN)
EMIT_D3D_OPCODE_DSS_FUNC(MAX)
EMIT_D3D_OPCODE_DSS_FUNC(SLT)
EMIT_D3D_OPCODE_DSS_FUNC(SGE)
EMIT_D3D_OPCODE_DS_FUNC(EXP)
EMIT_D3D_OPCODE_DS_FUNC(LOG)
EMIT_D3D_OPCODE_DS_FUNC(LIT)
EMIT_D3D_OPCODE_DSS_FUNC(DST)
EMIT_D3D_OPCODE_DSSS_FUNC(LRP)
EMIT_D3D_OPCODE_DS_FUNC(FRC)
EMIT_D3D_OPCODE_DSS_FUNC(M4X4)
EMIT_D3D_OPCODE_DSS_FUNC(M4X3)
EMIT_D3D_OPCODE_DSS_FUNC(M3X4)
EMIT_D3D_OPCODE_DSS_FUNC(M3X3)
EMIT_D3D_OPCODE_DSS_FUNC(M3X2)
EMIT_D3D_OPCODE_S_FUNC(CALL)
EMIT_D3D_OPCODE_SS_FUNC(CALLNZ)
EMIT_D3D_OPCODE_SS_FUNC(LOOP)
EMIT_D3D_OPCODE_FUNC(RET)
EMIT_D3D_OPCODE_FUNC(ENDLOOP)
EMIT_D3D_OPCODE_S_FUNC(LABEL)
EMIT_D3D_OPCODE_DSS_FUNC(POW)
EMIT_D3D_OPCODE_DSS_FUNC(CRS)
EMIT_D3D_OPCODE_DSSS_FUNC(SGN)
EMIT_D3D_OPCODE_DS_FUNC(ABS)
EMIT_D3D_OPCODE_DS_FUNC(NRM)
EMIT_D3D_OPCODE_S_FUNC(REP)
EMIT_D3D_OPCODE_FUNC(ENDREP)
EMIT_D3D_OPCODE_S_FUNC(IF)
EMIT_D3D_OPCODE_FUNC(ELSE)
EMIT_D3D_OPCODE_FUNC(ENDIF)
EMIT_D3D_OPCODE_FUNC(BREAK)
EMIT_D3D_OPCODE_DS_FUNC(MOVA)
EMIT_D3D_OPCODE_D_FUNC(TEXKILL)
EMIT_D3D_OPCODE_DS_FUNC(TEXBEM)
EMIT_D3D_OPCODE_DS_FUNC(TEXBEML)
EMIT_D3D_OPCODE_DS_FUNC(TEXREG2AR)
EMIT_D3D_OPCODE_DS_FUNC(TEXREG2GB)
EMIT_D3D_OPCODE_DS_FUNC(TEXM3X2PAD)
EMIT_D3D_OPCODE_DS_FUNC(TEXM3X2TEX)
EMIT_D3D_OPCODE_DS_FUNC(TEXM3X3PAD)
EMIT_D3D_OPCODE_DS_FUNC(TEXM3X3TEX)
EMIT_D3D_OPCODE_DSS_FUNC(TEXM3X3SPEC)
EMIT_D3D_OPCODE_DS_FUNC(TEXM3X3VSPEC)
EMIT_D3D_OPCODE_DS_FUNC(EXPP)
EMIT_D3D_OPCODE_DS_FUNC(LOGP)
EMIT_D3D_OPCODE_DSSS_FUNC(CND)
EMIT_D3D_OPCODE_DS_FUNC(TEXREG2RGB)
EMIT_D3D_OPCODE_DS_FUNC(TEXDP3TEX)
EMIT_D3D_OPCODE_DS_FUNC(TEXM3X2DEPTH)
EMIT_D3D_OPCODE_DS_FUNC(TEXDP3)
EMIT_D3D_OPCODE_DS_FUNC(TEXM3X3)
EMIT_D3D_OPCODE_D_FUNC(TEXDEPTH)
EMIT_D3D_OPCODE_DSSS_FUNC(CMP)
EMIT_D3D_OPCODE_DSS_FUNC(BEM)
EMIT_D3D_OPCODE_DSSS_FUNC(DP2ADD)
EMIT_D3D_OPCODE_DS_FUNC(DSX)
EMIT_D3D_OPCODE_DS_FUNC(DSY)
EMIT_D3D_OPCODE_DSSSS_FUNC(TEXLDD)
EMIT_D3D_OPCODE_DSS_FUNC(TEXLDL)
EMIT_D3D_OPCODE_S_FUNC(BREAKP)

// special cases for comparison opcodes...
static const char *get_D3D_comparison_string(Context *ctx)
{
    static const char *comps[] = {
        "", "_gt", "_eq", "_ge", "_lt", "_ne", "_le"
    };

    if (ctx->instruction_controls >= STATICARRAYLEN(comps))
    {
        fail(ctx, "unknown comparison control");
        return "";
    } // if

    return comps[ctx->instruction_controls];
} // get_D3D_comparison_string

static void emit_D3D_BREAKC(Context *ctx)
{
    char op[16];
    snprintf(op, sizeof (op), "break%s", get_D3D_comparison_string(ctx));
    emit_D3D_opcode_ss(ctx, op);
} // emit_D3D_BREAKC

static void emit_D3D_IFC(Context *ctx)
{
    char op[16];
    snprintf(op, sizeof (op), "if%s", get_D3D_comparison_string(ctx));
    emit_D3D_opcode_ss(ctx, op);
} // emit_D3D_IFC

static void emit_D3D_SETP(Context *ctx)
{
    char op[16];
    snprintf(op, sizeof (op), "setp%s", get_D3D_comparison_string(ctx));
    emit_D3D_opcode_dss(ctx, op);
} // emit_D3D_SETP

static void emit_D3D_DEF(Context *ctx)
{
    char dst[64];
    make_D3D_destarg_string(ctx, dst, sizeof (dst));
    const float *val = (const float *) ctx->dwords; // !!! FIXME: could be int?
    char val0[32];
    char val1[32];
    char val2[32];
    char val3[32];
    floatstr(ctx, val0, sizeof (val0), val[0], 0);
    floatstr(ctx, val1, sizeof (val1), val[1], 0);
    floatstr(ctx, val2, sizeof (val2), val[2], 0);
    floatstr(ctx, val3, sizeof (val3), val[3], 0);
    output_line(ctx, "def%s, %s, %s, %s, %s", dst, val0, val1, val2, val3);
} // emit_D3D_DEF

static void emit_D3D_DEFI(Context *ctx)
{
    char dst[64];
    make_D3D_destarg_string(ctx, dst, sizeof (dst));
    const int32 *x = (const int32 *) ctx->dwords;
    output_line(ctx, "defi%s, %d, %d, %d, %d", dst,
                (int) x[0], (int) x[1], (int) x[2], (int) x[3]);
} // emit_D3D_DEFI

static void emit_D3D_DEFB(Context *ctx)
{
    char dst[64];
    make_D3D_destarg_string(ctx, dst, sizeof (dst));
    output_line(ctx, "defb%s, %s", dst, ctx->dwords[0] ? "true" : "false");
} // emit_D3D_DEFB


static void emit_D3D_DCL(Context *ctx)
{
    char dst[64];
    make_D3D_destarg_string(ctx, dst, sizeof (dst));
    const DestArgInfo *arg = &ctx->dest_arg;
    const char *usage_str = "";
    char index_str[16] = { '\0' };

    if (arg->regtype == REG_TYPE_SAMPLER)
    {
        switch ((const TextureType) ctx->dwords[0])
        {
            case TEXTURE_TYPE_2D: usage_str = "_2d"; break;
            case TEXTURE_TYPE_CUBE: usage_str = "_cube"; break;
            case TEXTURE_TYPE_VOLUME: usage_str = "_volume"; break;
            default: fail(ctx, "unknown sampler texture type"); return;
        } // switch
    } // if

    else if (arg->regtype == REG_TYPE_MISCTYPE)
    {
        switch ((const MiscTypeType) arg->regnum)
        {
            case MISCTYPE_TYPE_POSITION:
            case MISCTYPE_TYPE_FACE:
                usage_str = "";  // just become "dcl vFace" or whatever.
                break;
            default: fail(ctx, "unknown misc register type"); return;
        } // switch
    } // else if

    else
    {
        const uint32 usage = ctx->dwords[0];
        const uint32 index = ctx->dwords[1];
        usage_str = usagestrs[usage];
        if (index != 0)
            snprintf(index_str, sizeof (index_str), "%u", (uint) index);
    } // else

    output_line(ctx, "dcl%s%s%s", usage_str, index_str, dst);
} // emit_D3D_DCL


static void emit_D3D_TEXCRD(Context *ctx)
{
    // this opcode looks and acts differently depending on the shader model.
    if (shader_version_atleast(ctx, 1, 4))
        emit_D3D_opcode_ds(ctx, "texcrd");
    else
        emit_D3D_opcode_d(ctx, "texcoord");
} // emit_D3D_TEXCOORD

static void emit_D3D_TEXLD(Context *ctx)
{
    // this opcode looks and acts differently depending on the shader model.
    if (shader_version_atleast(ctx, 2, 0))
    {
        if (ctx->instruction_controls == CONTROL_TEXLD)
           emit_D3D_opcode_dss(ctx, "texld");
        else if (ctx->instruction_controls == CONTROL_TEXLDP)
           emit_D3D_opcode_dss(ctx, "texldp");
        else if (ctx->instruction_controls == CONTROL_TEXLDB)
           emit_D3D_opcode_dss(ctx, "texldb");
    } // if

    else if (shader_version_atleast(ctx, 1, 4))
    {
        emit_D3D_opcode_ds(ctx, "texld");
    } // else if

    else
    {
        emit_D3D_opcode_d(ctx, "tex");
    } // else
} // emit_D3D_TEXLD

static void emit_D3D_SINCOS(Context *ctx)
{
    // this opcode needs extra registers for sm2 and lower.
    if (!shader_version_atleast(ctx, 3, 0))
        emit_D3D_opcode_dsss(ctx, "sincos");
    else
        emit_D3D_opcode_ds(ctx, "sincos");
} // emit_D3D_SINCOS


#undef EMIT_D3D_OPCODE_FUNC
#undef EMIT_D3D_OPCODE_D_FUNC
#undef EMIT_D3D_OPCODE_S_FUNC
#undef EMIT_D3D_OPCODE_SS_FUNC
#undef EMIT_D3D_OPCODE_DS_FUNC
#undef EMIT_D3D_OPCODE_DSS_FUNC
#undef EMIT_D3D_OPCODE_DSSS_FUNC
#undef EMIT_D3D_OPCODE_DSSSS_FUNC

#endif  // SUPPORT_PROFILE_D3D


#if !SUPPORT_PROFILE_BYTECODE
#define PROFILE_EMITTER_BYTECODE(op)
#else
#undef AT_LEAST_ONE_PROFILE
#define AT_LEAST_ONE_PROFILE 1
#define PROFILE_EMITTER_BYTECODE(op) emit_BYTECODE_##op,

static void emit_BYTECODE_start(Context *ctx, const char *profilestr)
{
    ctx->ignores_ctab = 1;
} // emit_BYTECODE_start

static void emit_BYTECODE_finalize(Context *ctx)
{
    // just copy the whole token stream and make all other emitters no-ops.
    if (set_output(ctx, &ctx->mainline))
    {
        const size_t len = ((size_t) (ctx->tokens - ctx->orig_tokens)) * sizeof (uint32);
        buffer_append(ctx->mainline, (const char *) ctx->orig_tokens, len);
    } // if
} // emit_BYTECODE_finalize

static void emit_BYTECODE_end(Context *ctx) {}
static void emit_BYTECODE_phase(Context *ctx) {}
static void emit_BYTECODE_global(Context *ctx, RegisterType t, int n) {}
static void emit_BYTECODE_array(Context *ctx, VariableList *var) {}
static void emit_BYTECODE_sampler(Context *c, int s, TextureType t, int tb) {}
static void emit_BYTECODE_const_array(Context *ctx, const ConstantsList *c,
                                         int base, int size) {}
static void emit_BYTECODE_uniform(Context *ctx, RegisterType t, int n,
                                  const VariableList *var) {}
static void emit_BYTECODE_attribute(Context *ctx, RegisterType t, int n,
                                       MOJOSHADER_usage u, int i, int w,
                                       int f) {}

static const char *get_BYTECODE_varname(Context *ctx, RegisterType rt, int regnum)
{
    char regnum_str[16];
    const char *regtype_str = get_D3D_register_string(ctx, rt, regnum,
                                              regnum_str, sizeof (regnum_str));
    char buf[64];
    snprintf(buf, sizeof (buf), "%s%s", regtype_str, regnum_str);
    return StrDup(ctx, buf);
} // get_BYTECODE_varname

static const char *get_BYTECODE_const_array_varname(Context *ctx, int base, int size)
{
    char buf[64];
    snprintf(buf, sizeof (buf), "c_array_%d_%d", base, size);
    return StrDup(ctx, buf);
} // get_BYTECODE_const_array_varname

#define EMIT_BYTECODE_OPCODE_FUNC(op) \
    static void emit_BYTECODE_##op(Context *ctx) {}

EMIT_BYTECODE_OPCODE_FUNC(RESERVED)
EMIT_BYTECODE_OPCODE_FUNC(NOP)
EMIT_BYTECODE_OPCODE_FUNC(MOV)
EMIT_BYTECODE_OPCODE_FUNC(ADD)
EMIT_BYTECODE_OPCODE_FUNC(SUB)
EMIT_BYTECODE_OPCODE_FUNC(MAD)
EMIT_BYTECODE_OPCODE_FUNC(MUL)
EMIT_BYTECODE_OPCODE_FUNC(RCP)
EMIT_BYTECODE_OPCODE_FUNC(RSQ)
EMIT_BYTECODE_OPCODE_FUNC(DP3)
EMIT_BYTECODE_OPCODE_FUNC(DP4)
EMIT_BYTECODE_OPCODE_FUNC(MIN)
EMIT_BYTECODE_OPCODE_FUNC(MAX)
EMIT_BYTECODE_OPCODE_FUNC(SLT)
EMIT_BYTECODE_OPCODE_FUNC(SGE)
EMIT_BYTECODE_OPCODE_FUNC(EXP)
EMIT_BYTECODE_OPCODE_FUNC(LOG)
EMIT_BYTECODE_OPCODE_FUNC(LIT)
EMIT_BYTECODE_OPCODE_FUNC(DST)
EMIT_BYTECODE_OPCODE_FUNC(LRP)
EMIT_BYTECODE_OPCODE_FUNC(FRC)
EMIT_BYTECODE_OPCODE_FUNC(M4X4)
EMIT_BYTECODE_OPCODE_FUNC(M4X3)
EMIT_BYTECODE_OPCODE_FUNC(M3X4)
EMIT_BYTECODE_OPCODE_FUNC(M3X3)
EMIT_BYTECODE_OPCODE_FUNC(M3X2)
EMIT_BYTECODE_OPCODE_FUNC(CALL)
EMIT_BYTECODE_OPCODE_FUNC(CALLNZ)
EMIT_BYTECODE_OPCODE_FUNC(LOOP)
EMIT_BYTECODE_OPCODE_FUNC(RET)
EMIT_BYTECODE_OPCODE_FUNC(ENDLOOP)
EMIT_BYTECODE_OPCODE_FUNC(LABEL)
EMIT_BYTECODE_OPCODE_FUNC(POW)
EMIT_BYTECODE_OPCODE_FUNC(CRS)
EMIT_BYTECODE_OPCODE_FUNC(SGN)
EMIT_BYTECODE_OPCODE_FUNC(ABS)
EMIT_BYTECODE_OPCODE_FUNC(NRM)
EMIT_BYTECODE_OPCODE_FUNC(SINCOS)
EMIT_BYTECODE_OPCODE_FUNC(REP)
EMIT_BYTECODE_OPCODE_FUNC(ENDREP)
EMIT_BYTECODE_OPCODE_FUNC(IF)
EMIT_BYTECODE_OPCODE_FUNC(ELSE)
EMIT_BYTECODE_OPCODE_FUNC(ENDIF)
EMIT_BYTECODE_OPCODE_FUNC(BREAK)
EMIT_BYTECODE_OPCODE_FUNC(MOVA)
EMIT_BYTECODE_OPCODE_FUNC(TEXKILL)
EMIT_BYTECODE_OPCODE_FUNC(TEXBEM)
EMIT_BYTECODE_OPCODE_FUNC(TEXBEML)
EMIT_BYTECODE_OPCODE_FUNC(TEXREG2AR)
EMIT_BYTECODE_OPCODE_FUNC(TEXREG2GB)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X2PAD)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X2TEX)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X3PAD)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X3TEX)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X3SPEC)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X3VSPEC)
EMIT_BYTECODE_OPCODE_FUNC(EXPP)
EMIT_BYTECODE_OPCODE_FUNC(LOGP)
EMIT_BYTECODE_OPCODE_FUNC(CND)
EMIT_BYTECODE_OPCODE_FUNC(TEXREG2RGB)
EMIT_BYTECODE_OPCODE_FUNC(TEXDP3TEX)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X2DEPTH)
EMIT_BYTECODE_OPCODE_FUNC(TEXDP3)
EMIT_BYTECODE_OPCODE_FUNC(TEXM3X3)
EMIT_BYTECODE_OPCODE_FUNC(TEXDEPTH)
EMIT_BYTECODE_OPCODE_FUNC(CMP)
EMIT_BYTECODE_OPCODE_FUNC(BEM)
EMIT_BYTECODE_OPCODE_FUNC(DP2ADD)
EMIT_BYTECODE_OPCODE_FUNC(DSX)
EMIT_BYTECODE_OPCODE_FUNC(DSY)
EMIT_BYTECODE_OPCODE_FUNC(TEXLDD)
EMIT_BYTECODE_OPCODE_FUNC(TEXLDL)
EMIT_BYTECODE_OPCODE_FUNC(BREAKP)
EMIT_BYTECODE_OPCODE_FUNC(BREAKC)
EMIT_BYTECODE_OPCODE_FUNC(IFC)
EMIT_BYTECODE_OPCODE_FUNC(SETP)
EMIT_BYTECODE_OPCODE_FUNC(DEF)
EMIT_BYTECODE_OPCODE_FUNC(DEFI)
EMIT_BYTECODE_OPCODE_FUNC(DEFB)
EMIT_BYTECODE_OPCODE_FUNC(DCL)
EMIT_BYTECODE_OPCODE_FUNC(TEXCRD)
EMIT_BYTECODE_OPCODE_FUNC(TEXLD)

#undef EMIT_BYTECODE_OPCODE_FUNC

#endif  // SUPPORT_PROFILE_BYTECODE


#if !SUPPORT_PROFILE_GLSL
#define PROFILE_EMITTER_GLSL(op)
#else
#undef AT_LEAST_ONE_PROFILE
#define AT_LEAST_ONE_PROFILE 1
#define PROFILE_EMITTER_GLSL(op) emit_GLSL_##op,

#define EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(op) \
    static void emit_GLSL_##op(Context *ctx) { \
        fail(ctx, #op " unimplemented in glsl profile"); \
    }

static inline const char *get_GLSL_register_string(Context *ctx,
                        const RegisterType regtype, const int regnum,
                        char *regnum_str, const size_t regnum_size)
{
    // turns out these are identical at the moment.
    return get_D3D_register_string(ctx,regtype,regnum,regnum_str,regnum_size);
} // get_GLSL_register_string

static const char *get_GLSL_uniform_type(Context *ctx, const RegisterType rtype)
{
    switch (rtype)
    {
        case REG_TYPE_CONST: return "vec4";
        case REG_TYPE_CONSTINT: return "ivec4";
        case REG_TYPE_CONSTBOOL: return "bool";
        default: fail(ctx, "BUG: used a uniform we don't know how to define.");
    } // switch

    return NULL;
} // get_GLSL_uniform_type

static const char *get_GLSL_varname_in_buf(Context *ctx, RegisterType rt,
                                           int regnum, char *buf,
                                           const size_t len)
{
    char regnum_str[16];
    const char *regtype_str = get_GLSL_register_string(ctx, rt, regnum,
                                              regnum_str, sizeof (regnum_str));
    snprintf(buf,len,"%s_%s%s", ctx->shader_type_str, regtype_str, regnum_str);
    return buf;
} // get_GLSL_varname_in_buf


static const char *get_GLSL_varname(Context *ctx, RegisterType rt, int regnum)
{
    char buf[64];
    get_GLSL_varname_in_buf(ctx, rt, regnum, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_GLSL_varname


static inline const char *get_GLSL_const_array_varname_in_buf(Context *ctx,
                                                const int base, const int size,
                                                char *buf, const size_t buflen)
{
    const char *type = ctx->shader_type_str;
    snprintf(buf, buflen, "%s_const_array_%d_%d", type, base, size);
    return buf;
} // get_GLSL_const_array_varname_in_buf

static const char *get_GLSL_const_array_varname(Context *ctx, int base, int size)
{
    char buf[64];
    get_GLSL_const_array_varname_in_buf(ctx, base, size, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_GLSL_const_array_varname


static inline const char *get_GLSL_input_array_varname(Context *ctx,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "%s", "vertex_input_array");
    return buf;
} // get_GLSL_input_array_varname


static const char *get_GLSL_uniform_array_varname(Context *ctx,
                                                  const RegisterType regtype,
                                                  char *buf, const size_t len)
{
    const char *shadertype = ctx->shader_type_str;
    const char *type = get_GLSL_uniform_type(ctx, regtype);
    snprintf(buf, len, "%s_uniforms_%s", shadertype, type);
    return buf;
} // get_GLSL_uniform_array_varname

static const char *get_GLSL_destarg_varname(Context *ctx, char *buf, size_t len)
{
    const DestArgInfo *arg = &ctx->dest_arg;
    return get_GLSL_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, len);
} // get_GLSL_destarg_varname

static const char *get_GLSL_srcarg_varname(Context *ctx, const size_t idx,
                                           char *buf, size_t len)
{
    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        *buf = '\0';
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];
    return get_GLSL_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, len);
} // get_GLSL_srcarg_varname


static const char *make_GLSL_destarg_assign(Context *, char *, const size_t,
                                            const char *, ...) ISPRINTF(4,5);

static const char *make_GLSL_destarg_assign(Context *ctx, char *buf,
                                            const size_t buflen,
                                            const char *fmt, ...)
{
    int need_parens = 0;
    const DestArgInfo *arg = &ctx->dest_arg;

    if (arg->writemask == 0)
    {
        *buf = '\0';
        return buf;  // no writemask? It's a no-op.
    } // if

    char clampbuf[32] = { '\0' };
    const char *clampleft = "";
    const char *clampright = "";
    if (arg->result_mod & MOD_SATURATE)
    {
        const int vecsize = vecsize_from_writemask(arg->writemask);
        clampleft = "clamp(";
        if (vecsize == 1)
            clampright = ", 0.0, 1.0)";
        else
        {
            snprintf(clampbuf, sizeof (clampbuf),
                     ", vec%d(0.0), vec%d(1.0))", vecsize, vecsize);
            clampright = clampbuf;
        } // else
    } // if

    // MSDN says MOD_PP is a hint and many implementations ignore it. So do we.

    // CENTROID only allowed in DCL opcodes, which shouldn't come through here.
    assert((arg->result_mod & MOD_CENTROID) == 0);

    if (ctx->predicated)
    {
        fail(ctx, "predicated destinations unsupported");  // !!! FIXME
        *buf = '\0';
        return buf;
    } // if

    char operation[256];
    va_list ap;
    va_start(ap, fmt);
    const int len = vsnprintf(operation, sizeof (operation), fmt, ap);
    va_end(ap);
    if (len >= sizeof (operation))
    {
        fail(ctx, "operation string too large");  // I'm lazy.  :P
        *buf = '\0';
        return buf;
    } // if

    const char *result_shift_str = "";
    switch (arg->result_shift)
    {
        case 0x1: result_shift_str = " * 2.0"; break;
        case 0x2: result_shift_str = " * 4.0"; break;
        case 0x3: result_shift_str = " * 8.0"; break;
        case 0xD: result_shift_str = " / 8.0"; break;
        case 0xE: result_shift_str = " / 4.0"; break;
        case 0xF: result_shift_str = " / 2.0"; break;
    } // switch
    need_parens |= (result_shift_str[0] != '\0');

    char regnum_str[16];
    const char *regtype_str = get_GLSL_register_string(ctx, arg->regtype,
                                                       arg->regnum, regnum_str,
                                                       sizeof (regnum_str));
    char writemask_str[6];
    size_t i = 0;
    const int scalar = isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum);
    if (!scalar && !writemask_xyzw(arg->writemask))
    {
        writemask_str[i++] = '.';
        if (arg->writemask0) writemask_str[i++] = 'x';
        if (arg->writemask1) writemask_str[i++] = 'y';
        if (arg->writemask2) writemask_str[i++] = 'z';
        if (arg->writemask3) writemask_str[i++] = 'w';
    } // if
    writemask_str[i] = '\0';
    assert(i < sizeof (writemask_str));

    const char *leftparen = (need_parens) ? "(" : "";
    const char *rightparen = (need_parens) ? ")" : "";

    snprintf(buf, buflen, "%s_%s%s%s = %s%s%s%s%s%s;",
             ctx->shader_type_str, regtype_str, regnum_str, writemask_str,
             clampleft, leftparen, operation, rightparen, result_shift_str,
             clampright);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_GLSL_destarg_assign


static char *make_GLSL_swizzle_string(char *swiz_str, const size_t strsize,
                                      const int swizzle, const int writemask)
{
    size_t i = 0;
    if ( (!no_swizzle(swizzle)) || (!writemask_xyzw(writemask)) )
    {
        const int writemask0 = (writemask >> 0) & 0x1;
        const int writemask1 = (writemask >> 1) & 0x1;
        const int writemask2 = (writemask >> 2) & 0x1;
        const int writemask3 = (writemask >> 3) & 0x1;

        const int swizzle_x = (swizzle >> 0) & 0x3;
        const int swizzle_y = (swizzle >> 2) & 0x3;
        const int swizzle_z = (swizzle >> 4) & 0x3;
        const int swizzle_w = (swizzle >> 6) & 0x3;

        swiz_str[i++] = '.';
        if (writemask0) swiz_str[i++] = swizzle_channels[swizzle_x];
        if (writemask1) swiz_str[i++] = swizzle_channels[swizzle_y];
        if (writemask2) swiz_str[i++] = swizzle_channels[swizzle_z];
        if (writemask3) swiz_str[i++] = swizzle_channels[swizzle_w];
    } // if
    assert(i < strsize);
    swiz_str[i] = '\0';
    return swiz_str;
} // make_GLSL_swizzle_string


static const char *make_GLSL_srcarg_string(Context *ctx, const size_t idx,
                                           const int writemask, char *buf,
                                           const size_t buflen)
{
    *buf = '\0';

    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];

    const char *premod_str = "";
    const char *postmod_str = "";
    switch (arg->src_mod)
    {
        case SRCMOD_NEGATE:
            premod_str = "-";
            break;

        case SRCMOD_BIASNEGATE:
            premod_str = "-(";
            postmod_str = " - 0.5)";
            break;

        case SRCMOD_BIAS:
            premod_str = "(";
            postmod_str = " - 0.5)";
            break;

        case SRCMOD_SIGNNEGATE:
            premod_str = "-((";
            postmod_str = " - 0.5) * 2.0)";
            break;

        case SRCMOD_SIGN:
            premod_str = "((";
            postmod_str = " - 0.5) * 2.0)";
            break;

        case SRCMOD_COMPLEMENT:
            premod_str = "(1.0 - ";
            postmod_str = ")";
            break;

        case SRCMOD_X2NEGATE:
            premod_str = "-(";
            postmod_str = " * 2.0)";
            break;

        case SRCMOD_X2:
            premod_str = "(";
            postmod_str = " * 2.0)";
            break;

        case SRCMOD_DZ:
            fail(ctx, "SRCMOD_DZ unsupported"); return buf; // !!! FIXME
            postmod_str = "_dz";
            break;

        case SRCMOD_DW:
            fail(ctx, "SRCMOD_DW unsupported"); return buf; // !!! FIXME
            postmod_str = "_dw";
            break;

        case SRCMOD_ABSNEGATE:
            premod_str = "-abs(";
            postmod_str = ")";
            break;

        case SRCMOD_ABS:
            premod_str = "abs(";
            postmod_str = ")";
            break;

        case SRCMOD_NOT:
            premod_str = "!";
            break;

        case SRCMOD_NONE:
        case SRCMOD_TOTAL:
             break;  // stop compiler whining.
    } // switch

    const char *regtype_str = NULL;

    if (!arg->relative)
    {
        regtype_str = get_GLSL_varname_in_buf(ctx, arg->regtype, arg->regnum,
                                              (char *) alloca(64), 64);
    } // if

    const char *rel_lbracket = "";
    char rel_offset[32] = { '\0' };
    const char *rel_rbracket = "";
    char rel_swizzle[4] = { '\0' };
    const char *rel_regtype_str = "";
    if (arg->relative)
    {
        if (arg->regtype == REG_TYPE_INPUT)
            regtype_str=get_GLSL_input_array_varname(ctx,(char*)alloca(64),64);
        else
        {
            assert(arg->regtype == REG_TYPE_CONST);
            const int arrayidx = arg->relative_array->index;
            const int offset = arg->regnum - arrayidx;
            assert(offset >= 0);
            if (arg->relative_array->constant)
            {
                const int arraysize = arg->relative_array->count;
                regtype_str = get_GLSL_const_array_varname_in_buf(ctx,
                                arrayidx, arraysize, (char *) alloca(64), 64);
                if (offset != 0)
                    snprintf(rel_offset, sizeof (rel_offset), "%d + ", offset);
            } // if
            else
            {
                regtype_str = get_GLSL_uniform_array_varname(ctx, arg->regtype,
                                                      (char *) alloca(64), 64);
                if (offset == 0)
                {
                    snprintf(rel_offset, sizeof (rel_offset),
                             "ARRAYBASE_%d + ", arrayidx);
                } // if
                else
                {
                    snprintf(rel_offset, sizeof (rel_offset),
                             "(ARRAYBASE_%d + %d) + ", arrayidx, offset);
                } // else
            } // else
        } // else

        rel_lbracket = "[";

        rel_regtype_str = get_GLSL_varname_in_buf(ctx, arg->relative_regtype,
                                                  arg->relative_regnum,
                                                  (char *) alloca(64), 64);
        rel_swizzle[0] = '.';
        rel_swizzle[1] = swizzle_channels[arg->relative_component];
        rel_swizzle[2] = '\0';
        rel_rbracket = "]";
    } // if

    char swiz_str[6] = { '\0' };
    if (!isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum))
    {
        make_GLSL_swizzle_string(swiz_str, sizeof (swiz_str),
                                 arg->swizzle, writemask);
    } // if

    if (regtype_str == NULL)
    {
        fail(ctx, "Unknown source register type.");
        return buf;
    } // if

    snprintf(buf, buflen, "%s%s%s%s%s%s%s%s%s",
             premod_str, regtype_str, rel_lbracket, rel_offset,
             rel_regtype_str, rel_swizzle, rel_rbracket, swiz_str,
             postmod_str);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_GLSL_srcarg_string

// generate some convenience functions.
#define MAKE_GLSL_SRCARG_STRING_(mask, bitmask) \
    static inline const char *make_GLSL_srcarg_string_##mask(Context *ctx, \
                                                const size_t idx, char *buf, \
                                                const size_t buflen) { \
        return make_GLSL_srcarg_string(ctx, idx, bitmask, buf, buflen); \
    }
MAKE_GLSL_SRCARG_STRING_(x, (1 << 0))
MAKE_GLSL_SRCARG_STRING_(y, (1 << 1))
MAKE_GLSL_SRCARG_STRING_(z, (1 << 2))
MAKE_GLSL_SRCARG_STRING_(w, (1 << 3))
MAKE_GLSL_SRCARG_STRING_(scalar, (1 << 0))
MAKE_GLSL_SRCARG_STRING_(full, 0xF)
MAKE_GLSL_SRCARG_STRING_(masked, ctx->dest_arg.writemask)
MAKE_GLSL_SRCARG_STRING_(vec3, 0x7)
MAKE_GLSL_SRCARG_STRING_(vec2, 0x3)
#undef MAKE_GLSL_SRCARG_STRING_

// special cases for comparison opcodes...

static const char *get_GLSL_comparison_string_scalar(Context *ctx)
{
    static const char *comps[] = { "", ">", "==", ">=", "<", "!=", "<=" };
    if (ctx->instruction_controls >= STATICARRAYLEN(comps))
    {
        fail(ctx, "unknown comparison control");
        return "";
    } // if

    return comps[ctx->instruction_controls];
} // get_GLSL_comparison_string_scalar

static const char *get_GLSL_comparison_string_vector(Context *ctx)
{
    static const char *comps[] = {
        "", "greaterThan", "equal", "greaterThanEqual", "lessThan",
        "notEqual", "lessThanEqual"
    };

    if (ctx->instruction_controls >= STATICARRAYLEN(comps))
    {
        fail(ctx, "unknown comparison control");
        return "";
    } // if

    return comps[ctx->instruction_controls];
} // get_GLSL_comparison_string_vector


static void emit_GLSL_start(Context *ctx, const char *profilestr)
{
    if (!shader_is_vertex(ctx) && !shader_is_pixel(ctx))
    {
        failf(ctx, "Shader type %u unsupported in this profile.",
              (uint) ctx->shader_type);
        return;
    } // if

    else if (strcmp(profilestr, MOJOSHADER_PROFILE_GLSL) == 0)
    {
        // No gl_FragData[] before GLSL 1.10, so we have to force the version.
        push_output(ctx, &ctx->preflight);
        output_line(ctx, "#version 110");
        pop_output(ctx);
    } // else if

    #if SUPPORT_PROFILE_GLSL120
    else if (strcmp(profilestr, MOJOSHADER_PROFILE_GLSL120) == 0)
    {
        ctx->profile_supports_glsl120 = 1;
        push_output(ctx, &ctx->preflight);
        output_line(ctx, "#version 120");
        pop_output(ctx);
    } // else if
    #endif

    #if SUPPORT_PROFILE_GLSLES
    else if (strcmp(profilestr, MOJOSHADER_PROFILE_GLSLES) == 0)
    {
        ctx->profile_supports_glsles = 1;
        push_output(ctx, &ctx->preflight);
        output_line(ctx, "#version 100");
        if (shader_is_vertex(ctx))
            output_line(ctx, "precision highp float;");
        else
            output_line(ctx, "precision mediump float;");
        output_line(ctx, "precision mediump int;");
        pop_output(ctx);
    } // else if
    #endif

    else
    {
        failf(ctx, "Profile '%s' unsupported or unknown.", profilestr);
        return;
    } // else

    push_output(ctx, &ctx->mainline_intro);
    output_line(ctx, "void main()");
    output_line(ctx, "{");
    pop_output(ctx);

    set_output(ctx, &ctx->mainline);
    ctx->indent++;
} // emit_GLSL_start

static void emit_GLSL_RET(Context *ctx);
static void emit_GLSL_end(Context *ctx)
{
    // ps_1_* writes color to r0 instead oC0. We move it to the right place.
    // We don't have to worry about a RET opcode messing this up, since
    //  RET isn't available before ps_2_0.
    if (shader_is_pixel(ctx) && !shader_version_atleast(ctx, 2, 0))
    {
        const char *shstr = ctx->shader_type_str;
        set_used_register(ctx, REG_TYPE_COLOROUT, 0, 1);
        output_line(ctx, "%s_oC0 = %s_r0;", shstr, shstr);
    } // if
    else if (shader_is_vertex(ctx))
    {
#ifdef MOJOSHADER_FLIP_RENDERTARGET
        output_line(ctx, "gl_Position.y = gl_Position.y * vpFlip;");
#endif
#ifdef MOJOSHADER_DEPTH_CLIPPING
        output_line(ctx, "gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;");
#endif
    } // else if

    // force a RET opcode if we're at the end of the stream without one.
    if (ctx->previous_opcode != OPCODE_RET)
        emit_GLSL_RET(ctx);
} // emit_GLSL_end

static void emit_GLSL_phase(Context *ctx)
{
    // no-op in GLSL.
} // emit_GLSL_phase

static void output_GLSL_uniform_array(Context *ctx, const RegisterType regtype,
                                      const int size)
{
    if (size > 0)
    {
        char buf[64];
        get_GLSL_uniform_array_varname(ctx, regtype, buf, sizeof (buf));
        const char *typ;
        switch (regtype)
        {
            case REG_TYPE_CONST: typ = "vec4"; break;
            case REG_TYPE_CONSTINT: typ ="ivec4"; break;
            case REG_TYPE_CONSTBOOL: typ = "bool"; break;
            default:
            {
                fail(ctx, "BUG: used a uniform we don't know how to define.");
                return;
            } // default
        } // switch
        output_line(ctx, "uniform %s %s[%d];", typ, buf, size);
    } // if
} // output_GLSL_uniform_array

static void emit_GLSL_finalize(Context *ctx)
{
    // throw some blank lines around to make source more readable.
    push_output(ctx, &ctx->globals);
    output_blank_line(ctx);
    pop_output(ctx);

    // If we had a relative addressing of REG_TYPE_INPUT, we need to build
    //  an array for it at the start of main(). GLSL doesn't let you specify
    //  arrays of attributes.
    //vec4 blah_array[BIGGEST_ARRAY];
    if (ctx->have_relative_input_registers) // !!! FIXME
        fail(ctx, "Relative addressing of input registers not supported.");

    push_output(ctx, &ctx->preflight);
    output_GLSL_uniform_array(ctx, REG_TYPE_CONST, ctx->uniform_float4_count);
    output_GLSL_uniform_array(ctx, REG_TYPE_CONSTINT, ctx->uniform_int4_count);
    output_GLSL_uniform_array(ctx, REG_TYPE_CONSTBOOL, ctx->uniform_bool_count);
#ifdef MOJOSHADER_FLIP_RENDERTARGET
    if (shader_is_vertex(ctx))
        output_line(ctx, "uniform float vpFlip;");
#endif
    pop_output(ctx);
} // emit_GLSL_finalize

static void emit_GLSL_global(Context *ctx, RegisterType regtype, int regnum)
{
    char varname[64];
    get_GLSL_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    push_output(ctx, &ctx->globals);
    switch (regtype)
    {
        case REG_TYPE_ADDRESS:
            if (shader_is_vertex(ctx))
                output_line(ctx, "ivec4 %s;", varname);
            else if (shader_is_pixel(ctx))  // actually REG_TYPE_TEXTURE.
            {
                // We have to map texture registers to temps for ps_1_1, since
                //  they work like temps, initialize with tex coords, and the
                //  ps_1_1 TEX opcode expects to overwrite it.
                if (!shader_version_atleast(ctx, 1, 4))
                {
#if SUPPORT_PROFILE_GLSLES
                    // GLSL ES does not have gl_TexCoord
                    if (support_glsles(ctx))
                        output_line(ctx, "vec4 %s = io_%i_%i;",
                                    varname, MOJOSHADER_USAGE_TEXCOORD, regnum);
                    else
#endif
                    output_line(ctx, "vec4 %s = gl_TexCoord[%d];",
                                varname, regnum);
                } // if
            } // else if
            break;
        case REG_TYPE_PREDICATE:
            output_line(ctx, "bvec4 %s;", varname);
            break;
        case REG_TYPE_TEMP:
            output_line(ctx, "vec4 %s;", varname);
            break;
        case REG_TYPE_LOOP:
            break; // no-op. We declare these in for loops at the moment.
        case REG_TYPE_LABEL:
            break; // no-op. If we see it here, it means we optimized it out.
        default:
            fail(ctx, "BUG: we used a register we don't know how to define.");
            break;
    } // switch
    pop_output(ctx);
} // emit_GLSL_global

static void emit_GLSL_array(Context *ctx, VariableList *var)
{
    // All uniforms (except constant arrays, which only get pushed once at
    //  compile time) are now packed into a single array, so we can batch
    //  the uniform transfers. So this doesn't actually define an array
    //  here; the one, big array is emitted during finalization instead.
    // However, we need to #define the offset into the one, big array here,
    //  and let dereferences use that #define.
    const int base = var->index;
    const int glslbase = ctx->uniform_float4_count;
    push_output(ctx, &ctx->globals);
    output_line(ctx, "#define ARRAYBASE_%d %d", base, glslbase);
    pop_output(ctx);
    var->emit_position = glslbase;
} // emit_GLSL_array

static void emit_GLSL_const_array(Context *ctx, const ConstantsList *clist,
                                  int base, int size)
{
    char varname[64];
    get_GLSL_const_array_varname_in_buf(ctx,base,size,varname,sizeof(varname));

#if 0
    // !!! FIXME: fails on Nvidia's and Apple's GL, even with #version 120.
    // !!! FIXME:  (the 1.20 spec says it should work, though, I think...)
    if (support_glsl120(ctx))
    {
        // GLSL 1.20 can do constant arrays.
        const char *cstr = NULL;
        push_output(ctx, &ctx->globals);
        output_line(ctx, "const vec4 %s[%d] = vec4[%d](", varname, size, size);
        ctx->indent++;

        int i;
        for (i = 0; i < size; i++)
        {
            while (clist->constant.type != MOJOSHADER_UNIFORM_FLOAT)
                clist = clist->next;
            assert(clist->constant.index == (base + i));

            char val0[32];
            char val1[32];
            char val2[32];
            char val3[32];
            floatstr(ctx, val0, sizeof (val0), clist->constant.value.f[0], 1);
            floatstr(ctx, val1, sizeof (val1), clist->constant.value.f[1], 1);
            floatstr(ctx, val2, sizeof (val2), clist->constant.value.f[2], 1);
            floatstr(ctx, val3, sizeof (val3), clist->constant.value.f[3], 1);

            output_line(ctx, "vec4(%s, %s, %s, %s)%s", val0, val1, val2, val3,
                        (i < (size-1)) ? "," : "");

            clist = clist->next;
        } // for

        ctx->indent--;
        output_line(ctx, ");");
        pop_output(ctx);
    } // if

    else
#endif
    {
        // stock GLSL 1.0 can't do constant arrays, so make a uniform array
        //  and have the OpenGL glue assign it at link time. Lame!
        push_output(ctx, &ctx->globals);
        output_line(ctx, "uniform vec4 %s[%d];", varname, size);
        pop_output(ctx);
    } // else
} // emit_GLSL_const_array

static void emit_GLSL_uniform(Context *ctx, RegisterType regtype, int regnum,
                              const VariableList *var)
{
    // Now that we're pushing all the uniforms as one big array, pack these
    //  down, so if we only use register c439, it'll actually map to
    //  glsl_uniforms_vec4[0]. As we push one big array, this will prevent
    //  uploading unused data.

    char varname[64];
    char name[64];
    int index = 0;

    get_GLSL_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    push_output(ctx, &ctx->globals);

    if (var == NULL)
    {
        get_GLSL_uniform_array_varname(ctx, regtype, name, sizeof (name));

        if (regtype == REG_TYPE_CONST)
            index = ctx->uniform_float4_count;
        else if (regtype == REG_TYPE_CONSTINT)
            index = ctx->uniform_int4_count;
        else if (regtype == REG_TYPE_CONSTBOOL)
            index = ctx->uniform_bool_count;
        else  // get_GLSL_uniform_array_varname() would have called fail().
            assert(isfail(ctx));

        output_line(ctx, "#define %s %s[%d]", varname, name, index);
    } // if

    else
    {
        const int arraybase = var->index;
        if (var->constant)
        {
            get_GLSL_const_array_varname_in_buf(ctx, arraybase, var->count,
                                                name, sizeof (name));
            index = (regnum - arraybase);
        } // if
        else
        {
            assert(var->emit_position != -1);
            get_GLSL_uniform_array_varname(ctx, regtype, name, sizeof (name));
            index = (regnum - arraybase) + var->emit_position;
        } // else

        output_line(ctx, "#define %s %s[%d]", varname, name, index);
    } // else

    pop_output(ctx);
} // emit_GLSL_uniform

static void emit_GLSL_sampler(Context *ctx,int stage,TextureType ttype,int tb)
{
    const char *type = "";
    switch (ttype)
    {
        case TEXTURE_TYPE_2D: type = "sampler2D"; break;
        case TEXTURE_TYPE_CUBE: type = "samplerCube"; break;
        case TEXTURE_TYPE_VOLUME: type = "sampler3D"; break;
        default: fail(ctx, "BUG: used a sampler we don't know how to define.");
    } // switch

    char var[64];
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, stage, var, sizeof (var));

    push_output(ctx, &ctx->globals);
    output_line(ctx, "uniform %s %s;", type, var);
    if (tb)  // This sampler used a ps_1_1 TEXBEM opcode?
    {
        char name[64];
        const int index = ctx->uniform_float4_count;
        ctx->uniform_float4_count += 2;
        get_GLSL_uniform_array_varname(ctx, REG_TYPE_CONST, name, sizeof (name));
        output_line(ctx, "#define %s_texbem %s[%d]", var, name, index);
        output_line(ctx, "#define %s_texbeml %s[%d]", var, name, index+1);
    } // if
    pop_output(ctx);
} // emit_GLSL_sampler

static void emit_GLSL_attribute(Context *ctx, RegisterType regtype, int regnum,
                                MOJOSHADER_usage usage, int index, int wmask,
                                int flags)
{
    // !!! FIXME: this function doesn't deal with write masks at all yet!
    const char *usage_str = NULL;
    const char *arrayleft = "";
    const char *arrayright = "";
    char index_str[16] = { '\0' };
    char var[64];

    get_GLSL_varname_in_buf(ctx, regtype, regnum, var, sizeof (var));

    //assert((flags & MOD_PP) == 0);  // !!! FIXME: is PP allowed?

    if (index != 0)  // !!! FIXME: a lot of these MUST be zero.
        snprintf(index_str, sizeof (index_str), "%u", (uint) index);

    if (shader_is_vertex(ctx))
    {
        // pre-vs3 output registers.
        // these don't ever happen in DCL opcodes, I think. Map to vs_3_*
        //  output registers.
        if (!shader_version_atleast(ctx, 3, 0))
        {
            if (regtype == REG_TYPE_RASTOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                index = regnum;
                switch ((const RastOutType) regnum)
                {
                    case RASTOUT_TYPE_POSITION:
                        usage = MOJOSHADER_USAGE_POSITION;
                        break;
                    case RASTOUT_TYPE_FOG:
                        usage = MOJOSHADER_USAGE_FOG;
                        break;
                    case RASTOUT_TYPE_POINT_SIZE:
                        usage = MOJOSHADER_USAGE_POINTSIZE;
                        break;
                } // switch
            } // if

            else if (regtype == REG_TYPE_ATTROUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_COLOR;
                index = regnum;
            } // else if

            else if (regtype == REG_TYPE_TEXCRDOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_TEXCOORD;
                index = regnum;
            } // else if
        } // if

        // to avoid limitations of various GL entry points for input
        // attributes (glSecondaryColorPointer() can only take 3 component
        // items, glVertexPointer() can't do GL_UNSIGNED_BYTE, many other
        // issues), we set up all inputs as generic vertex attributes, so we
        // can pass data in just about any form, and ignore the built-in GLSL
        // attributes like gl_SecondaryColor. Output needs to use the the
        // built-ins, though, but we don't have to worry about the GL entry
        // point limitations there.

        if (regtype == REG_TYPE_INPUT)
        {
            push_output(ctx, &ctx->globals);
            output_line(ctx, "attribute vec4 %s;", var);
            pop_output(ctx);
        } // if

        else if (regtype == REG_TYPE_OUTPUT)
        {
            switch (usage)
            {
                case MOJOSHADER_USAGE_POSITION:
                    usage_str = "gl_Position";
                    break;
                case MOJOSHADER_USAGE_POINTSIZE:
                    usage_str = "gl_PointSize";
                    break;
                case MOJOSHADER_USAGE_COLOR:
#if SUPPORT_PROFILE_GLSLES
                    if (support_glsles(ctx))
                        break; // GLSL ES does not have gl_FrontColor
#endif
                    index_str[0] = '\0';  // no explicit number.
                    if (index == 0)
                    {
                        usage_str = "gl_FrontColor";
                    } // if
                    else if (index == 1)
                    {
                        usage_str = "gl_FrontSecondaryColor";
                    } // else if
                    break;
                case MOJOSHADER_USAGE_FOG:
                    usage_str = "gl_FogFragCoord";
                    break;
                case MOJOSHADER_USAGE_TEXCOORD:
#if SUPPORT_PROFILE_GLSLES
                    if (support_glsles(ctx))
                        break; // GLSL ES does not have gl_TexCoord
#endif
                    snprintf(index_str, sizeof (index_str), "%u", (uint) index);
                    usage_str = "gl_TexCoord";
                    arrayleft = "[";
                    arrayright = "]";
                    break;
                default:
                    // !!! FIXME: we need to deal with some more built-in varyings here.
                    break;
            } // switch

            // !!! FIXME: the #define is a little hacky, but it means we don't
            // !!! FIXME:  have to track these separately if this works.
            push_output(ctx, &ctx->globals);
            // no mapping to built-in var? Just make it a regular global, pray.
            if (usage_str == NULL)
            {
#if SUPPORT_PROFILE_GLSLES
                if (support_glsles(ctx))
                    output_line(ctx, "varying highp vec4 io_%i_%i;", usage, index);
                else
#endif
                output_line(ctx, "varying vec4 io_%i_%i;", usage, index);
                output_line(ctx, "#define %s io_%i_%i", var, usage, index);
            } // if
            else
            {
                output_line(ctx, "#define %s %s%s%s%s", var, usage_str,
                            arrayleft, index_str, arrayright);
            } // else
            pop_output(ctx);
        } // else if

        else
        {
            fail(ctx, "unknown vertex shader attribute register");
        } // else
    } // if

    else if (shader_is_pixel(ctx))
    {
        // samplers DCLs get handled in emit_GLSL_sampler().

        if (flags & MOD_CENTROID)  // !!! FIXME
        {
            failf(ctx, "centroid unsupported in %s profile", ctx->profile->name);
            return;
        } // if

        if (regtype == REG_TYPE_COLOROUT)
        {
            if (!ctx->have_multi_color_outputs)
                usage_str = "gl_FragColor";  // maybe faster?
            else
            {
                snprintf(index_str, sizeof (index_str), "%u", (uint) regnum);
                usage_str = "gl_FragData";
                arrayleft = "[";
                arrayright = "]";
            } // else
        } // if

        else if (regtype == REG_TYPE_DEPTHOUT)
            usage_str = "gl_FragDepth";

        // !!! FIXME: can you actualy have a texture register with COLOR usage?
        else if ((regtype == REG_TYPE_TEXTURE) || (regtype == REG_TYPE_INPUT))
        {
#if SUPPORT_PROFILE_GLSLES
            if (!support_glsles(ctx))
            {
#endif
            if (usage == MOJOSHADER_USAGE_TEXCOORD)
            {
                // ps_1_1 does a different hack for this attribute.
                //  Refer to emit_GLSL_global()'s REG_TYPE_ADDRESS code.
                if (shader_version_atleast(ctx, 1, 4))
                {
                    snprintf(index_str, sizeof (index_str), "%u", (uint) index);
                    usage_str = "gl_TexCoord";
                    arrayleft = "[";
                    arrayright = "]";
                } // if
            } // if

            else if (usage == MOJOSHADER_USAGE_COLOR)
            {
                index_str[0] = '\0';  // no explicit number.
                if (index == 0)
                {
                    usage_str = "gl_Color";
                } // if
                else if (index == 1)
                {
                    usage_str = "gl_SecondaryColor";
                } // else if
                // FIXME: Does this even matter when we have varyings? -flibit
                // else
                //    fail(ctx, "unsupported color index");
            } // else if
#if SUPPORT_PROFILE_GLSLES
            } // if
#endif
        } // else if

        else if (regtype == REG_TYPE_MISCTYPE)
        {
            const MiscTypeType mt = (MiscTypeType) regnum;
            if (mt == MISCTYPE_TYPE_FACE)
            {
                push_output(ctx, &ctx->globals);
                output_line(ctx, "float %s = gl_FrontFacing ? 1.0 : -1.0;", var);
                pop_output(ctx);
            } // if
            else if (mt == MISCTYPE_TYPE_POSITION)
            {
                index_str[0] = '\0';  // no explicit number.
                usage_str = "gl_FragCoord";  // !!! FIXME: is this the same coord space as D3D?
            } // else if
            else
            {
                fail(ctx, "BUG: unhandled misc register");
            } // else
        } // else if

        else
        {
            fail(ctx, "unknown pixel shader attribute register");
        } // else

        push_output(ctx, &ctx->globals);
        // no mapping to built-in var? Just make it a regular global, pray.
        if (usage_str == NULL)
        {
#if SUPPORT_PROFILE_GLSLES
            if (support_glsles(ctx))
                output_line(ctx, "varying highp vec4 io_%i_%i;", usage, index);
            else
#endif
            output_line(ctx, "varying vec4 io_%i_%i;", usage, index);
            output_line(ctx, "#define %s io_%i_%i", var, usage, index);
        } // if
        else
        {
            output_line(ctx, "#define %s %s%s%s%s", var, usage_str,
                        arrayleft, index_str, arrayright);
        } // else
        pop_output(ctx);
    } // else if

    else
    {
        fail(ctx, "Unknown shader type");  // state machine should catch this.
    } // else
} // emit_GLSL_attribute

static void emit_GLSL_NOP(Context *ctx)
{
    // no-op is a no-op.  :)
} // emit_GLSL_NOP

static void emit_GLSL_MOV(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_MOV

static void emit_GLSL_ADD(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s + %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_ADD

static void emit_GLSL_SUB(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s - %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_SUB

static void emit_GLSL_MAD(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_GLSL_srcarg_string_masked(ctx, 2, src2, sizeof (src2));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "(%s * %s) + %s", src0, src1, src2);
    output_line(ctx, "%s", code);
} // emit_GLSL_MAD

static void emit_GLSL_MUL(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%s * %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_MUL

static void emit_GLSL_RCP(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "1.0 / %s", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_RCP

static void emit_GLSL_RSQ(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "inversesqrt(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_RSQ

static void emit_GLSL_dotprod(Context *ctx, const char *src0, const char *src1,
                              const char *extra)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char castleft[16] = { '\0' };
    const char *castright = "";
    if (vecsize != 1)
    {
        snprintf(castleft, sizeof (castleft), "vec%d(", vecsize);
        castright = ")";
    } // if

    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "%sdot(%s, %s)%s%s",
                             castleft, src0, src1, extra, castright);
    output_line(ctx, "%s", code);
} // emit_GLSL_dotprod

static void emit_GLSL_DP3(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_vec3(ctx, 1, src1, sizeof (src1));
    emit_GLSL_dotprod(ctx, src0, src1, "");
} // emit_GLSL_DP3

static void emit_GLSL_DP4(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_full(ctx, 1, src1, sizeof (src1));
    emit_GLSL_dotprod(ctx, src0, src1, "");
} // emit_GLSL_DP4

static void emit_GLSL_MIN(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "min(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_MIN

static void emit_GLSL_MAX(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "max(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_MAX

static void emit_GLSL_SLT(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];

    // float(bool) or vec(bvec) results in 0.0 or 1.0, like SLT wants.
    if (vecsize == 1)
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "float(%s < %s)", src0, src1);
    else
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "vec%d(lessThan(%s, %s))",
                                 vecsize, src0, src1);
    } // else
    output_line(ctx, "%s", code);
} // emit_GLSL_SLT

static void emit_GLSL_SGE(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];

    // float(bool) or vec(bvec) results in 0.0 or 1.0, like SGE wants.
    if (vecsize == 1)
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "float(%s >= %s)", src0, src1);
    } // if
    else
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "vec%d(greaterThanEqual(%s, %s))",
                                 vecsize, src0, src1);
    } // else
    output_line(ctx, "%s", code);
} // emit_GLSL_SGE

static void emit_GLSL_EXP(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "exp2(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_EXP

static void emit_GLSL_LOG(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "log2(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_LOG

static void emit_GLSL_LIT_helper(Context *ctx)
{
    const char *maxp = "127.9961"; // value from the dx9 reference.

    if (ctx->glsl_generated_lit_helper)
        return;

    ctx->glsl_generated_lit_helper = 1;

    push_output(ctx, &ctx->helpers);
    output_line(ctx, "vec4 LIT(const vec4 src)");
    output_line(ctx, "{"); ctx->indent++;
    output_line(ctx,   "float power = clamp(src.w, -%s, %s);",maxp,maxp);
    output_line(ctx,   "vec4 retval = vec4(1.0, 0.0, 0.0, 1.0);");
    output_line(ctx,   "if (src.x > 0.0) {"); ctx->indent++;
    output_line(ctx,     "retval.y = src.x;");
    output_line(ctx,     "if (src.y > 0.0) {"); ctx->indent++;
    output_line(ctx,       "retval.z = pow(src.y, power);"); ctx->indent--;
    output_line(ctx,     "}"); ctx->indent--;
    output_line(ctx,   "}");
    output_line(ctx,   "return retval;"); ctx->indent--;
    output_line(ctx, "}");
    output_blank_line(ctx);
    pop_output(ctx);
} // emit_GLSL_LIT_helper

static void emit_GLSL_LIT(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char code[128];
    emit_GLSL_LIT_helper(ctx);
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "LIT(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_LIT

static void emit_GLSL_DST(Context *ctx)
{
    // !!! FIXME: needs to take ctx->dst_arg.writemask into account.
    char src0_y[64]; make_GLSL_srcarg_string_y(ctx, 0, src0_y, sizeof (src0_y));
    char src1_y[64]; make_GLSL_srcarg_string_y(ctx, 1, src1_y, sizeof (src1_y));
    char src0_z[64]; make_GLSL_srcarg_string_z(ctx, 0, src0_z, sizeof (src0_z));
    char src1_w[64]; make_GLSL_srcarg_string_w(ctx, 1, src1_w, sizeof (src1_w));

    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                             "vec4(1.0, %s * %s, %s, %s)",
                             src0_y, src1_y, src0_z, src1_w);
    output_line(ctx, "%s", code);
} // emit_GLSL_DST

static void emit_GLSL_LRP(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_GLSL_srcarg_string_masked(ctx, 2, src2, sizeof (src2));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "mix(%s, %s, %s)",
                             src2, src1, src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_LRP

static void emit_GLSL_FRC(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "fract(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_FRC

static void emit_GLSL_M4X4(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_GLSL_srcarg_string_full(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_GLSL_srcarg_string_full(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_GLSL_srcarg_string_full(ctx, 3, row2, sizeof (row2));
    char row3[64]; make_GLSL_srcarg_string_full(ctx, 4, row3, sizeof (row3));
    char code[256];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                    "vec4(dot(%s, %s), dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                    src0, row0, src0, row1, src0, row2, src0, row3);
    output_line(ctx, "%s", code);
} // emit_GLSL_M4X4

static void emit_GLSL_M4X3(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_GLSL_srcarg_string_full(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_GLSL_srcarg_string_full(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_GLSL_srcarg_string_full(ctx, 3, row2, sizeof (row2));
    char code[256];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "vec3(dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1, src0, row2);
    output_line(ctx, "%s", code);
} // emit_GLSL_M4X3

static void emit_GLSL_M3X4(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_GLSL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_GLSL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_GLSL_srcarg_string_vec3(ctx, 3, row2, sizeof (row2));
    char row3[64]; make_GLSL_srcarg_string_vec3(ctx, 4, row3, sizeof (row3));

    char code[256];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "vec4(dot(%s, %s), dot(%s, %s), "
                                     "dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1,
                                src0, row2, src0, row3);
    output_line(ctx, "%s", code);
} // emit_GLSL_M3X4

static void emit_GLSL_M3X3(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_GLSL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_GLSL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_GLSL_srcarg_string_vec3(ctx, 3, row2, sizeof (row2));
    char code[256];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "vec3(dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1, src0, row2);
    output_line(ctx, "%s", code);
} // emit_GLSL_M3X3

static void emit_GLSL_M3X2(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_GLSL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_GLSL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));

    char code[256];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                "vec2(dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1);
    output_line(ctx, "%s", code);
} // emit_GLSL_M3X2

static void emit_GLSL_CALL(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    if (ctx->loops > 0)
        output_line(ctx, "%s(aL);", src0);
    else
        output_line(ctx, "%s();", src0);
} // emit_GLSL_CALL

static void emit_GLSL_CALLNZ(Context *ctx)
{
    // !!! FIXME: if src1 is a constbool that's true, we can remove the
    // !!! FIXME:  if. If it's false, we can make this a no-op.
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));

    if (ctx->loops > 0)
        output_line(ctx, "if (%s) { %s(aL); }", src1, src0);
    else
        output_line(ctx, "if (%s) { %s(); }", src1, src0);
} // emit_GLSL_CALLNZ

static void emit_GLSL_LOOP(Context *ctx)
{
    // !!! FIXME: swizzle?
    char var[64]; get_GLSL_srcarg_varname(ctx, 1, var, sizeof (var));
    assert(ctx->source_args[0].regnum == 0);  // in case they add aL1 someday.
    output_line(ctx, "{");
    ctx->indent++;
    output_line(ctx, "const int aLend = %s.x + %s.y;", var, var);
    output_line(ctx, "for (int aL = %s.y; aL < aLend; aL += %s.z) {", var, var);
    ctx->indent++;
} // emit_GLSL_LOOP

static void emit_GLSL_RET(Context *ctx)
{
    // thankfully, the MSDN specs say a RET _has_ to end a function...no
    //  early returns. So if you hit one, you know you can safely close
    //  a high-level function.
    ctx->indent--;
    output_line(ctx, "}");
    output_blank_line(ctx);
    set_output(ctx, &ctx->subroutines);  // !!! FIXME: is this for LABEL? Maybe set it there so we don't allocate unnecessarily.
} // emit_GLSL_RET

static void emit_GLSL_ENDLOOP(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
    ctx->indent--;
    output_line(ctx, "}");
} // emit_GLSL_ENDLOOP

static void emit_GLSL_LABEL(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    const int label = ctx->source_args[0].regnum;
    RegisterList *reg = reglist_find(&ctx->used_registers, REG_TYPE_LABEL, label);
    assert(ctx->output == ctx->subroutines);  // not mainline, etc.
    assert(ctx->indent == 0);  // we shouldn't be in the middle of a function.

    // MSDN specs say CALL* has to come before the LABEL, so we know if we
    //  can ditch the entire function here as unused.
    if (reg == NULL)
        set_output(ctx, &ctx->ignore);  // Func not used. Parse, but don't output.

    // !!! FIXME: it would be nice if we could determine if a function is
    // !!! FIXME:  only called once and, if so, forcibly inline it.

    const char *uses_loopreg = ((reg) && (reg->misc == 1)) ? "int aL" : "";
    output_line(ctx, "void %s(%s)", src0, uses_loopreg);
    output_line(ctx, "{");
    ctx->indent++;
} // emit_GLSL_LABEL

static void emit_GLSL_DCL(Context *ctx)
{
    // no-op. We do this in our emit_attribute() and emit_uniform().
} // emit_GLSL_DCL

static void emit_GLSL_POW(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                             "pow(abs(%s), %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_POW

static void emit_GLSL_CRS(Context *ctx)
{
    // !!! FIXME: needs to take ctx->dst_arg.writemask into account.
    char src0[64]; make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_vec3(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code),
                             "cross(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_GLSL_CRS

static void emit_GLSL_SGN(Context *ctx)
{
    // (we don't need the temporary registers specified for the D3D opcode.)
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "sign(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_SGN

static void emit_GLSL_ABS(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "abs(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_ABS

static void emit_GLSL_NRM(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "normalize(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_NRM

static void emit_GLSL_SINCOS(Context *ctx)
{
    // we don't care about the temp registers that <= sm2 demands; ignore them.
    //  sm2 also talks about what components are left untouched vs. undefined,
    //  but we just leave those all untouched with GLSL write masks (which
    //  would fulfill the "undefined" requirement, too).
    const int mask = ctx->dest_arg.writemask;
    char src0[64]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char code[128] = { '\0' };

    if (writemask_x(mask))
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "cos(%s)", src0);
    else if (writemask_y(mask))
        make_GLSL_destarg_assign(ctx, code, sizeof (code), "sin(%s)", src0);
    else if (writemask_xy(mask))
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "vec2(cos(%s), sin(%s))", src0, src0);
    } // else if

    output_line(ctx, "%s", code);
} // emit_GLSL_SINCOS

static void emit_GLSL_REP(Context *ctx)
{
    // !!! FIXME:
    // msdn docs say legal loop values are 0 to 255. We can check DEFI values
    //  at parse time, but if they are pulling a value from a uniform, do
    //  we clamp here?
    // !!! FIXME: swizzle is legal here, right?
    char src0[64]; make_GLSL_srcarg_string_x(ctx, 0, src0, sizeof (src0));
    const uint rep = (uint) ctx->reps;
    output_line(ctx, "for (int rep%u = 0; rep%u < %s; rep%u++) {",
                rep, rep, src0, rep);
    ctx->indent++;
} // emit_GLSL_REP

static void emit_GLSL_ENDREP(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
} // emit_GLSL_ENDREP

static void emit_GLSL_IF(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    output_line(ctx, "if (%s) {", src0);
    ctx->indent++;
} // emit_GLSL_IF

static void emit_GLSL_IFC(Context *ctx)
{
    const char *comp = get_GLSL_comparison_string_scalar(ctx);
    char src0[64]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "if (%s %s %s) {", src0, comp, src1);
    ctx->indent++;
} // emit_GLSL_IFC

static void emit_GLSL_ELSE(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "} else {");
    ctx->indent++;
} // emit_GLSL_ELSE

static void emit_GLSL_ENDIF(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
} // emit_GLSL_ENDIF

static void emit_GLSL_BREAK(Context *ctx)
{
    output_line(ctx, "break;");
} // emit_GLSL_BREAK

static void emit_GLSL_BREAKC(Context *ctx)
{
    const char *comp = get_GLSL_comparison_string_scalar(ctx);
    char src0[64]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "if (%s %s %s) { break; }", src0, comp, src1);
} // emit_GLSL_BREAKC

static void emit_GLSL_MOVA(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];

    if (vecsize == 1)
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "int(floor(abs(%s) + 0.5) * sign(%s))",
                                 src0, src0);
    } // if

    else
    {
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                            "ivec%d(floor(abs(%s) + vec%d(0.5)) * sign(%s))",
                            vecsize, src0, vecsize, src0);
    } // else

    output_line(ctx, "%s", code);
} // emit_GLSL_MOVA

static void emit_GLSL_DEFB(Context *ctx)
{
    char varname[64]; get_GLSL_destarg_varname(ctx, varname, sizeof (varname));
    push_output(ctx, &ctx->globals);
    output_line(ctx, "const bool %s = %s;",
                varname, ctx->dwords[0] ? "true" : "false");
    pop_output(ctx);
} // emit_GLSL_DEFB

static void emit_GLSL_DEFI(Context *ctx)
{
    char varname[64]; get_GLSL_destarg_varname(ctx, varname, sizeof (varname));
    const int32 *x = (const int32 *) ctx->dwords;
    push_output(ctx, &ctx->globals);
    output_line(ctx, "const ivec4 %s = ivec4(%d, %d, %d, %d);",
                varname, (int) x[0], (int) x[1], (int) x[2], (int) x[3]);
    pop_output(ctx);
} // emit_GLSL_DEFI

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXCRD)

static void emit_GLSL_TEXKILL(Context *ctx)
{
    char dst[64]; get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
    output_line(ctx, "if (any(lessThan(%s.xyz, vec3(0.0)))) discard;", dst);
} // emit_GLSL_TEXKILL

static void glsl_texld(Context *ctx, const int texldd)
{
    if (!shader_version_atleast(ctx, 1, 4))
    {
        DestArgInfo *info = &ctx->dest_arg;
        char dst[64];
        char sampler[64];
        char code[128] = {0};

        assert(!texldd);

        RegisterList *sreg;
        sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER, info->regnum);
        const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);

        // !!! FIXME: this code counts on the register not having swizzles, etc.
        get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
        get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                                sampler, sizeof (sampler));

        if (ttype == TEXTURE_TYPE_2D)
        {
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "texture2D(%s, %s.xy)",
                                     sampler, dst);
        }
        else if (ttype == TEXTURE_TYPE_CUBE)
        {
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "textureCube(%s, %s.xyz)",
                                     sampler, dst);
        }
        else if (ttype == TEXTURE_TYPE_VOLUME)
        {
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "texture3D(%s, %s.xyz)",
                                     sampler, dst);
        }
        else
        {
            fail(ctx, "unexpected texture type");
        } // else
        output_line(ctx, "%s", code);
    } // if

    else if (!shader_version_atleast(ctx, 2, 0))
    {
        // ps_1_4 is different, too!
        fail(ctx, "TEXLD == Shader Model 1.4 unimplemented.");  // !!! FIXME
        return;
    } // else if

    else
    {
        const SourceArgInfo *samp_arg = &ctx->source_args[1];
        RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                          samp_arg->regnum);
        const char *funcname = NULL;
        char src0[64] = { '\0' };
        char src1[64]; get_GLSL_srcarg_varname(ctx, 1, src1, sizeof (src1)); // !!! FIXME: SRC_MOD?
        char src2[64] = { '\0' };
        char src3[64] = { '\0' };

        if (sreg == NULL)
        {
            fail(ctx, "TEXLD using undeclared sampler");
            return;
        } // if

        if (texldd)
        {
            if (sreg->index == TEXTURE_TYPE_2D)
            {
                make_GLSL_srcarg_string_vec2(ctx, 2, src2, sizeof (src2));
                make_GLSL_srcarg_string_vec2(ctx, 3, src3, sizeof (src3));
            } // if
            else
            {
                assert((sreg->index == TEXTURE_TYPE_CUBE) || (sreg->index == TEXTURE_TYPE_VOLUME));
                make_GLSL_srcarg_string_vec3(ctx, 2, src2, sizeof (src2));
                make_GLSL_srcarg_string_vec3(ctx, 3, src3, sizeof (src3));
            } // else
        } // if

        // !!! FIXME: can TEXLDD set instruction_controls?
        // !!! FIXME: does the d3d bias value map directly to GLSL?
        const char *biassep = "";
        char bias[64] = { '\0' };
        if (ctx->instruction_controls == CONTROL_TEXLDB)
        {
            biassep = ", ";
            make_GLSL_srcarg_string_w(ctx, 0, bias, sizeof (bias));
        } // if

        switch ((const TextureType) sreg->index)
        {
            case TEXTURE_TYPE_2D:
                if (ctx->instruction_controls == CONTROL_TEXLDP)
                {
                    funcname = "texture2DProj";
                    make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
                } // if
                else  // texld/texldb
                {
                    funcname = "texture2D";
                    make_GLSL_srcarg_string_vec2(ctx, 0, src0, sizeof (src0));
                } // else
                break;
            case TEXTURE_TYPE_CUBE:
                if (ctx->instruction_controls == CONTROL_TEXLDP)
                    fail(ctx, "TEXLDP on a cubemap");  // !!! FIXME: is this legal?
                funcname = "textureCube";
                make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
                break;
            case TEXTURE_TYPE_VOLUME:
                if (ctx->instruction_controls == CONTROL_TEXLDP)
                {
                    funcname = "texture3DProj";
                    make_GLSL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
                } // if
                else  // texld/texldb
                {
                    funcname = "texture3D";
                    make_GLSL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
                } // else
                break;
            default:
                fail(ctx, "unknown texture type");
                return;
        } // switch

        assert(!isscalar(ctx, ctx->shader_type, samp_arg->regtype, samp_arg->regnum));
        char swiz_str[6] = { '\0' };
        make_GLSL_swizzle_string(swiz_str, sizeof (swiz_str),
                                 samp_arg->swizzle, ctx->dest_arg.writemask);

        char code[128];
        if (texldd)
        {
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "%sGrad(%s, %s, %s, %s)%s", funcname,
                                     src1, src0, src2, src3, swiz_str);
        } // if
        else
        {
            make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                     "%s(%s, %s%s%s)%s", funcname,
                                     src1, src0, biassep, bias, swiz_str);
        } // else

        output_line(ctx, "%s", code);
    } // else
} // glsl_texld

static void emit_GLSL_TEXLD(Context *ctx)
{
    glsl_texld(ctx, 0);
} // emit_GLSL_TEXLD
    

static void emit_GLSL_TEXBEM(Context *ctx)
{
    DestArgInfo *info = &ctx->dest_arg;
    char dst[64]; get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
    char src[64]; get_GLSL_srcarg_varname(ctx, 0, src, sizeof (src));
    char sampler[64];
    char code[512];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture2D(%s, vec2(%s.x + (%s_texbem.x * %s.x) + (%s_texbem.z * %s.y),"
        " %s.y + (%s_texbem.y * %s.x) + (%s_texbem.w * %s.y)))",
        sampler,
        dst, sampler, src, sampler, src,
        dst, sampler, src, sampler, src);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXBEM


static void emit_GLSL_TEXBEML(Context *ctx)
{
    // !!! FIXME: this code counts on the register not having swizzles, etc.
    DestArgInfo *info = &ctx->dest_arg;
    char dst[64]; get_GLSL_destarg_varname(ctx, dst, sizeof (dst));
    char src[64]; get_GLSL_srcarg_varname(ctx, 0, src, sizeof (src));
    char sampler[64];
    char code[512];

    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "(texture2D(%s, vec2(%s.x + (%s_texbem.x * %s.x) + (%s_texbem.z * %s.y),"
        " %s.y + (%s_texbem.y * %s.x) + (%s_texbem.w * %s.y)))) *"
        " ((%s.z * %s_texbeml.x) + %s_texbem.y)",
        sampler,
        dst, sampler, src, sampler, src,
        dst, sampler, src, sampler, src,
        src, sampler, sampler);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXBEML

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2AR) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2GB) // !!! FIXME


static void emit_GLSL_TEXM3X2PAD(Context *ctx)
{
    // no-op ... work happens in emit_GLSL_TEXM3X2TEX().
} // emit_GLSL_TEXM3X2PAD

static void emit_GLSL_TEXM3X2TEX(Context *ctx)
{
    if (ctx->texm3x2pad_src0 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char sampler[64];
    char code[512];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_src0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_dst0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src2, sizeof (src2));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture2D(%s, vec2(dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz)))",
        sampler, src0, src1, src2, dst);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X2TEX

static void emit_GLSL_TEXM3X3PAD(Context *ctx)
{
    // no-op ... work happens in emit_GLSL_TEXM3X3*().
} // emit_GLSL_TEXM3X3PAD

static void emit_GLSL_TEXM3X3TEX(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char sampler[64];
    char code[512];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "Cube" : "3D";

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture%s(%s,"
            " vec3(dot(%s.xyz, %s.xyz),"
            " dot(%s.xyz, %s.xyz),"
            " dot(%s.xyz, %s.xyz)))",
        ttypestr, sampler, src0, src1, src2, src3, dst, src4);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3TEX

static void emit_GLSL_TEXM3X3SPEC_helper(Context *ctx)
{
    if (ctx->glsl_generated_texm3x3spec_helper)
        return;

    ctx->glsl_generated_texm3x3spec_helper = 1;

    push_output(ctx, &ctx->helpers);
    output_line(ctx, "vec3 TEXM3X3SPEC_reflection(const vec3 normal, const vec3 eyeray)");
    output_line(ctx, "{"); ctx->indent++;
    output_line(ctx,   "return (2.0 * ((normal * eyeray) / (normal * normal)) * normal) - eyeray;"); ctx->indent--;
    output_line(ctx, "}");
    output_blank_line(ctx);
    pop_output(ctx);
} // emit_GLSL_TEXM3X3SPEC_helper

static void emit_GLSL_TEXM3X3SPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char src5[64];
    char sampler[64];
    char code[512];

    emit_GLSL_TEXM3X3SPEC_helper(ctx);

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[1].regnum,
                            src5, sizeof (src5));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "Cube" : "3D";

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture%s(%s, "
            "TEXM3X3SPEC_reflection("
                "vec3("
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz)"
                "),"
                "%s.xyz,"
            ")"
        ")",
        ttypestr, sampler, src0, src1, src2, src3, dst, src4, src5);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3SPEC

static void emit_GLSL_TEXM3X3VSPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char sampler[64];
    char code[512];

    emit_GLSL_TEXM3X3SPEC_helper(ctx);

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "Cube" : "3D";

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "texture%s(%s, "
            "TEXM3X3SPEC_reflection("
                "vec3("
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz)"
                "), "
                "vec3(%s.w, %s.w, %s.w)"
            ")"
        ")",
        ttypestr, sampler, src0, src1, src2, src3, dst, src4, src0, src2, dst);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3VSPEC

static void emit_GLSL_EXPP(Context *ctx)
{
    // !!! FIXME: msdn's asm docs don't list this opcode, I'll have to check the driver documentation.
    emit_GLSL_EXP(ctx);  // I guess this is just partial precision EXP?
} // emit_GLSL_EXPP

static void emit_GLSL_LOGP(Context *ctx)
{
    // LOGP is just low-precision LOG, but we'll take the higher precision.
    emit_GLSL_LOG(ctx);
} // emit_GLSL_LOGP

// common code between CMP and CND.
static void emit_GLSL_comparison_operations(Context *ctx, const char *cmp)
{
    int i, j;
    DestArgInfo *dst = &ctx->dest_arg;
    const SourceArgInfo *srcarg0 = &ctx->source_args[0];
    const int origmask = dst->writemask;
    int used_swiz[4] = { 0, 0, 0, 0 };
    const int writemask[4] = { dst->writemask0, dst->writemask1,
                               dst->writemask2, dst->writemask3 };
    const int src0swiz[4] = { srcarg0->swizzle_x, srcarg0->swizzle_y,
                              srcarg0->swizzle_z, srcarg0->swizzle_w };

    for (i = 0; i < 4; i++)
    {
        int mask = (1 << i);

        if (!writemask[i]) continue;
        if (used_swiz[i]) continue;

        // This is a swizzle we haven't checked yet.
        used_swiz[i] = 1;

        // see if there are any other elements swizzled to match (.yyyy)
        for (j = i + 1; j < 4; j++)
        {
            if (!writemask[j]) continue;
            if (src0swiz[i] != src0swiz[j]) continue;
            mask |= (1 << j);
            used_swiz[j] = 1;
        } // for

        // okay, (mask) should be the writemask of swizzles we like.

        //return make_GLSL_srcarg_string(ctx, idx, (1 << 0));

        char src0[64];
        char src1[64];
        char src2[64];
        make_GLSL_srcarg_string(ctx, 0, (1 << i), src0, sizeof (src0));
        make_GLSL_srcarg_string(ctx, 1, mask, src1, sizeof (src1));
        make_GLSL_srcarg_string(ctx, 2, mask, src2, sizeof (src2));

        set_dstarg_writemask(dst, mask);

        char code[128];
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "((%s %s) ? %s : %s)",
                                 src0, cmp, src1, src2);
        output_line(ctx, "%s", code);
    } // for

    set_dstarg_writemask(dst, origmask);
} // emit_GLSL_comparison_operations

static void emit_GLSL_CND(Context *ctx)
{
    emit_GLSL_comparison_operations(ctx, "> 0.5");
} // emit_GLSL_CND

static void emit_GLSL_DEF(Context *ctx)
{
    const float *val = (const float *) ctx->dwords; // !!! FIXME: could be int?
    char varname[64]; get_GLSL_destarg_varname(ctx, varname, sizeof (varname));
    char val0[32]; floatstr(ctx, val0, sizeof (val0), val[0], 1);
    char val1[32]; floatstr(ctx, val1, sizeof (val1), val[1], 1);
    char val2[32]; floatstr(ctx, val2, sizeof (val2), val[2], 1);
    char val3[32]; floatstr(ctx, val3, sizeof (val3), val[3], 1);

    push_output(ctx, &ctx->globals);
    output_line(ctx, "const vec4 %s = vec4(%s, %s, %s, %s);",
                varname, val0, val1, val2, val3);
    pop_output(ctx);
} // emit_GLSL_DEF

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2RGB) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3TEX) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X2DEPTH) // !!! FIXME
EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3) // !!! FIXME

static void emit_GLSL_TEXM3X3(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char code[512];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_GLSL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_GLSL_destarg_varname(ctx, dst, sizeof (dst));

    make_GLSL_destarg_assign(ctx, code, sizeof (code),
        "vec4(dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz), 1.0)",
        src0, src1, src2, src3, dst, src4);

    output_line(ctx, "%s", code);
} // emit_GLSL_TEXM3X3

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(TEXDEPTH) // !!! FIXME

static void emit_GLSL_CMP(Context *ctx)
{
    emit_GLSL_comparison_operations(ctx, ">= 0.0");
} // emit_GLSL_CMP

EMIT_GLSL_OPCODE_UNIMPLEMENTED_FUNC(BEM) // !!! FIXME

static void emit_GLSL_DP2ADD(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_vec2(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_vec2(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_GLSL_srcarg_string_scalar(ctx, 2, src2, sizeof (src2));
    char extra[64]; snprintf(extra, sizeof (extra), " + %s", src2);
    emit_GLSL_dotprod(ctx, src0, src1, extra);
} // emit_GLSL_DP2ADD

static void emit_GLSL_DSX(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "dFdx(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_DSX

static void emit_GLSL_DSY(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_GLSL_destarg_assign(ctx, code, sizeof (code), "dFdy(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_GLSL_DSY

static void emit_GLSL_TEXLDD(Context *ctx)
{
    // !!! FIXME:
    // GLSL 1.30 introduced textureGrad() for this, but it looks like the
    //  functions are overloaded instead of texture2DGrad() (etc).

    // GL_shader_texture_lod and GL_EXT_gpu_shader4 added texture2DGrad*(),
    //  so we'll use them if available. Failing that, we'll just fallback
    //  to a regular texture2D call and hope the mipmap it chooses is close
    //  enough.
    if (!ctx->glsl_generated_texldd_setup)
    {
        ctx->glsl_generated_texldd_setup = 1;
        push_output(ctx, &ctx->preflight);
        output_line(ctx, "#if GL_ARB_shader_texture_lod");
        output_line(ctx, "#extension GL_ARB_shader_texture_lod : enable");
        output_line(ctx, "#define texture2DGrad texture2DGradARB");
        output_line(ctx, "#define texture2DProjGrad texture2DProjARB");
        output_line(ctx, "#elif GL_EXT_gpu_shader4");
        output_line(ctx, "#extension GL_EXT_gpu_shader4 : enable");
        output_line(ctx, "#else");
        output_line(ctx, "#define texture2DGrad(a,b,c,d) texture2D(a,b)");
        output_line(ctx, "#define texture2DProjGrad(a,b,c,d) texture2DProj(a,b)");
        output_line(ctx, "#endif");
        output_blank_line(ctx);
        pop_output(ctx);
    } // if

    glsl_texld(ctx, 1);
} // emit_GLSL_TEXLDD

static void emit_GLSL_SETP(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_GLSL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_GLSL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];

    // destination is always predicate register (which is type bvec4).
    if (vecsize == 1)
    {
        const char *comp = get_GLSL_comparison_string_scalar(ctx);
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "(%s %s %s)", src0, comp, src1);
    } // if
    else
    {
        const char *comp = get_GLSL_comparison_string_vector(ctx);
        make_GLSL_destarg_assign(ctx, code, sizeof (code),
                                 "%s(%s, %s)", comp, src0, src1);
    } // else

    output_line(ctx, "%s", code);
} // emit_GLSL_SETP

static void emit_GLSL_TEXLDL(Context *ctx)
{
    // !!! FIXME: The spec says we can't use GLSL's texture*Lod() built-ins
    // !!! FIXME:  from fragment shaders for some inexplicable reason.
    // !!! FIXME:  For now, you'll just have to suffer with the potentially
    // !!! FIXME:  wrong mipmap until I can figure something out.
    emit_GLSL_TEXLD(ctx);
} // emit_GLSL_TEXLDL

static void emit_GLSL_BREAKP(Context *ctx)
{
    char src0[64]; make_GLSL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    output_line(ctx, "if (%s) { break; }", src0);
} // emit_GLSL_BREAKP

static void emit_GLSL_RESERVED(Context *ctx)
{
    // do nothing; fails in the state machine.
} // emit_GLSL_RESERVED

#endif  // SUPPORT_PROFILE_GLSL


// !!! FIXME: A lot of this is cut-and-paste from the GLSL version.
#if !SUPPORT_PROFILE_METAL
#define PROFILE_EMITTER_METAL(op)
#else
#undef AT_LEAST_ONE_PROFILE
#define AT_LEAST_ONE_PROFILE 1
#define PROFILE_EMITTER_METAL(op) emit_METAL_##op,

#define EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(op) \
    static void emit_METAL_##op(Context *ctx) { \
        fail(ctx, #op " unimplemented in Metal profile"); \
    }

static inline const char *get_METAL_register_string(Context *ctx,
                        const RegisterType regtype, const int regnum,
                        char *regnum_str, const size_t regnum_size)
{
    // turns out these are identical at the moment.
    return get_D3D_register_string(ctx,regtype,regnum,regnum_str,regnum_size);
} // get_METAL_register_string

static const char *get_METAL_uniform_type(Context *ctx, const RegisterType rtype)
{
    switch (rtype)
    {
        case REG_TYPE_CONST: return "float4";
        case REG_TYPE_CONSTINT: return "int4";
        case REG_TYPE_CONSTBOOL: return "bool";
        default: fail(ctx, "BUG: used a uniform we don't know how to define.");
    } // switch

    return NULL;
} // get_METAL_uniform_type

static const char *get_METAL_varname_in_buf(Context *ctx, RegisterType rt,
                                           int regnum, char *buf,
                                           const size_t len)
{
    char regnum_str[16];
    const char *regtype_str = get_METAL_register_string(ctx, rt, regnum,
                                              regnum_str, sizeof (regnum_str));

    // We don't separate vars with vs_ or ps_ here, because, for the most part,
    //  there are only local vars in Metal shaders.
    snprintf(buf, len, "%s%s", regtype_str, regnum_str);
    return buf;
} // get_METAL_varname_in_buf


static const char *get_METAL_varname(Context *ctx, RegisterType rt, int regnum)
{
    char buf[64];
    get_METAL_varname_in_buf(ctx, rt, regnum, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_METAL_varname


static inline const char *get_METAL_const_array_varname_in_buf(Context *ctx,
                                                const int base, const int size,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "const_array_%d_%d", base, size);
    return buf;
} // get_METAL_const_array_varname_in_buf

static const char *get_METAL_const_array_varname(Context *ctx, int base, int size)
{
    char buf[64];
    get_METAL_const_array_varname_in_buf(ctx, base, size, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_METAL_const_array_varname


static inline const char *get_METAL_input_array_varname(Context *ctx,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "%s", "vertex_input_array");
    return buf;
} // get_METAL_input_array_varname


static const char *get_METAL_uniform_array_varname(Context *ctx,
                                                  const RegisterType regtype,
                                                  char *buf, const size_t len)
{
    const char *shadertype = ctx->shader_type_str;
    const char *type = get_METAL_uniform_type(ctx, regtype);
    snprintf(buf, len, "uniforms.uniforms_%s", type);
    return buf;
} // get_METAL_uniform_array_varname

static const char *get_METAL_destarg_varname(Context *ctx, char *buf, size_t len)
{
    const DestArgInfo *arg = &ctx->dest_arg;
    return get_METAL_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, len);
} // get_METAL_destarg_varname

static const char *get_METAL_srcarg_varname(Context *ctx, const size_t idx,
                                           char *buf, size_t len)
{
    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        *buf = '\0';
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];
    return get_METAL_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, len);
} // get_METAL_srcarg_varname


static const char *make_METAL_destarg_assign(Context *, char *, const size_t,
                                            const char *, ...) ISPRINTF(4,5);

static const char *make_METAL_destarg_assign(Context *ctx, char *buf,
                                            const size_t buflen,
                                            const char *fmt, ...)
{
    int need_parens = 0;
    const DestArgInfo *arg = &ctx->dest_arg;

    if (arg->writemask == 0)
    {
        *buf = '\0';
        return buf;  // no writemask? It's a no-op.
    } // if

    char clampbuf[32] = { '\0' };
    const char *clampleft = "";
    const char *clampright = "";
    if (arg->result_mod & MOD_SATURATE)
    {
        ctx->metal_need_header_common = 1;
        const int vecsize = vecsize_from_writemask(arg->writemask);
        clampleft = "clamp(";
        if (vecsize == 1)
            clampright = ", 0.0, 1.0)";
        else
        {
            snprintf(clampbuf, sizeof (clampbuf),
                     ", float%d(0.0), float%d(1.0))", vecsize, vecsize);
            clampright = clampbuf;
        } // else
    } // if

    // MSDN says MOD_PP is a hint and many implementations ignore it. So do we.

    // CENTROID only allowed in DCL opcodes, which shouldn't come through here.
    assert((arg->result_mod & MOD_CENTROID) == 0);

    if (ctx->predicated)
    {
        fail(ctx, "predicated destinations unsupported");  // !!! FIXME
        *buf = '\0';
        return buf;
    } // if

    char operation[256];
    va_list ap;
    va_start(ap, fmt);
    const int len = vsnprintf(operation, sizeof (operation), fmt, ap);
    va_end(ap);
    if (len >= sizeof (operation))
    {
        fail(ctx, "operation string too large");  // I'm lazy.  :P
        *buf = '\0';
        return buf;
    } // if

    const char *result_shift_str = "";
    switch (arg->result_shift)
    {
        case 0x1: result_shift_str = " * 2.0"; break;
        case 0x2: result_shift_str = " * 4.0"; break;
        case 0x3: result_shift_str = " * 8.0"; break;
        case 0xD: result_shift_str = " / 8.0"; break;
        case 0xE: result_shift_str = " / 4.0"; break;
        case 0xF: result_shift_str = " / 2.0"; break;
    } // switch
    need_parens |= (result_shift_str[0] != '\0');

    char regnum_str[16];
    const char *regtype_str = get_METAL_register_string(ctx, arg->regtype,
                                                       arg->regnum, regnum_str,
                                                       sizeof (regnum_str));
    char writemask_str[6];
    size_t i = 0;
    const int scalar = isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum);
    if (!scalar && !writemask_xyzw(arg->writemask))
    {
        writemask_str[i++] = '.';
        if (arg->writemask0) writemask_str[i++] = 'x';
        if (arg->writemask1) writemask_str[i++] = 'y';
        if (arg->writemask2) writemask_str[i++] = 'z';
        if (arg->writemask3) writemask_str[i++] = 'w';
    } // if
    writemask_str[i] = '\0';
    assert(i < sizeof (writemask_str));

    const char *leftparen = (need_parens) ? "(" : "";
    const char *rightparen = (need_parens) ? ")" : "";

    snprintf(buf, buflen, "%s%s%s = %s%s%s%s%s%s;",
             regtype_str, regnum_str, writemask_str,
             clampleft, leftparen, operation, rightparen, result_shift_str,
             clampright);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_METAL_destarg_assign


static char *make_METAL_swizzle_string(char *swiz_str, const size_t strsize,
                                      const int swizzle, const int writemask)
{
    size_t i = 0;
    if ( (!no_swizzle(swizzle)) || (!writemask_xyzw(writemask)) )
    {
        const int writemask0 = (writemask >> 0) & 0x1;
        const int writemask1 = (writemask >> 1) & 0x1;
        const int writemask2 = (writemask >> 2) & 0x1;
        const int writemask3 = (writemask >> 3) & 0x1;

        const int swizzle_x = (swizzle >> 0) & 0x3;
        const int swizzle_y = (swizzle >> 2) & 0x3;
        const int swizzle_z = (swizzle >> 4) & 0x3;
        const int swizzle_w = (swizzle >> 6) & 0x3;

        swiz_str[i++] = '.';
        if (writemask0) swiz_str[i++] = swizzle_channels[swizzle_x];
        if (writemask1) swiz_str[i++] = swizzle_channels[swizzle_y];
        if (writemask2) swiz_str[i++] = swizzle_channels[swizzle_z];
        if (writemask3) swiz_str[i++] = swizzle_channels[swizzle_w];
    } // if
    assert(i < strsize);
    swiz_str[i] = '\0';
    return swiz_str;
} // make_METAL_swizzle_string


static const char *make_METAL_srcarg_string(Context *ctx, const size_t idx,
                                           const int writemask, char *buf,
                                           const size_t buflen)
{
    *buf = '\0';

    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];

    const char *premod_str = "";
    const char *postmod_str = "";
    switch (arg->src_mod)
    {
        case SRCMOD_NEGATE:
            premod_str = "-";
            break;

        case SRCMOD_BIASNEGATE:
            premod_str = "-(";
            postmod_str = " - 0.5)";
            break;

        case SRCMOD_BIAS:
            premod_str = "(";
            postmod_str = " - 0.5)";
            break;

        case SRCMOD_SIGNNEGATE:
            premod_str = "-((";
            postmod_str = " - 0.5) * 2.0)";
            break;

        case SRCMOD_SIGN:
            premod_str = "((";
            postmod_str = " - 0.5) * 2.0)";
            break;

        case SRCMOD_COMPLEMENT:
            premod_str = "(1.0 - ";
            postmod_str = ")";
            break;

        case SRCMOD_X2NEGATE:
            premod_str = "-(";
            postmod_str = " * 2.0)";
            break;

        case SRCMOD_X2:
            premod_str = "(";
            postmod_str = " * 2.0)";
            break;

        case SRCMOD_DZ:
            fail(ctx, "SRCMOD_DZ unsupported"); return buf; // !!! FIXME
            postmod_str = "_dz";
            break;

        case SRCMOD_DW:
            fail(ctx, "SRCMOD_DW unsupported"); return buf; // !!! FIXME
            postmod_str = "_dw";
            break;

        case SRCMOD_ABSNEGATE:
            ctx->metal_need_header_math = 1;
            premod_str = "-abs(";
            postmod_str = ")";
            break;

        case SRCMOD_ABS:
            ctx->metal_need_header_math = 1;
            premod_str = "abs(";
            postmod_str = ")";
            break;

        case SRCMOD_NOT:
            premod_str = "!";
            break;

        case SRCMOD_NONE:
        case SRCMOD_TOTAL:
             break;  // stop compiler whining.
    } // switch

    const char *regtype_str = NULL;

    if (!arg->relative)
    {
        regtype_str = get_METAL_varname_in_buf(ctx, arg->regtype, arg->regnum,
                                              (char *) alloca(64), 64);
    } // if

    const char *rel_lbracket = "";
    char rel_offset[32] = { '\0' };
    const char *rel_rbracket = "";
    char rel_swizzle[4] = { '\0' };
    const char *rel_regtype_str = "";
    if (arg->relative)
    {
        if (arg->regtype == REG_TYPE_INPUT)
            regtype_str=get_METAL_input_array_varname(ctx,(char*)alloca(64),64);
        else
        {
            assert(arg->regtype == REG_TYPE_CONST);
            const int arrayidx = arg->relative_array->index;
            const int offset = arg->regnum - arrayidx;
            assert(offset >= 0);
            if (arg->relative_array->constant)
            {
                const int arraysize = arg->relative_array->count;
                regtype_str = get_METAL_const_array_varname_in_buf(ctx,
                                arrayidx, arraysize, (char *) alloca(64), 64);
                if (offset != 0)
                    snprintf(rel_offset, sizeof (rel_offset), "%d + ", offset);
            } // if
            else
            {
                regtype_str = get_METAL_uniform_array_varname(ctx, arg->regtype,
                                                      (char *) alloca(64), 64);
                if (offset == 0)
                {
                    snprintf(rel_offset, sizeof (rel_offset),
                             "ARRAYBASE_%d + ", arrayidx);
                } // if
                else
                {
                    snprintf(rel_offset, sizeof (rel_offset),
                             "(ARRAYBASE_%d + %d) + ", arrayidx, offset);
                } // else
            } // else
        } // else

        rel_lbracket = "[";

        rel_regtype_str = get_METAL_varname_in_buf(ctx, arg->relative_regtype,
                                                  arg->relative_regnum,
                                                  (char *) alloca(64), 64);
        rel_swizzle[0] = '.';
        rel_swizzle[1] = swizzle_channels[arg->relative_component];
        rel_swizzle[2] = '\0';
        rel_rbracket = "]";
    } // if

    char swiz_str[6] = { '\0' };
    if (!isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum))
    {
        make_METAL_swizzle_string(swiz_str, sizeof (swiz_str),
                                 arg->swizzle, writemask);
    } // if

    if (regtype_str == NULL)
    {
        fail(ctx, "Unknown source register type.");
        return buf;
    } // if

    snprintf(buf, buflen, "%s%s%s%s%s%s%s%s%s",
             premod_str, regtype_str, rel_lbracket, rel_offset,
             rel_regtype_str, rel_swizzle, rel_rbracket, swiz_str,
             postmod_str);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_METAL_srcarg_string

// generate some convenience functions.
#define MAKE_METAL_SRCARG_STRING_(mask, bitmask) \
    static inline const char *make_METAL_srcarg_string_##mask(Context *ctx, \
                                                const size_t idx, char *buf, \
                                                const size_t buflen) { \
        return make_METAL_srcarg_string(ctx, idx, bitmask, buf, buflen); \
    }
MAKE_METAL_SRCARG_STRING_(x, (1 << 0))
MAKE_METAL_SRCARG_STRING_(y, (1 << 1))
MAKE_METAL_SRCARG_STRING_(z, (1 << 2))
MAKE_METAL_SRCARG_STRING_(w, (1 << 3))
MAKE_METAL_SRCARG_STRING_(scalar, (1 << 0))
MAKE_METAL_SRCARG_STRING_(full, 0xF)
MAKE_METAL_SRCARG_STRING_(masked, ctx->dest_arg.writemask)
MAKE_METAL_SRCARG_STRING_(vec3, 0x7)
MAKE_METAL_SRCARG_STRING_(vec2, 0x3)
#undef MAKE_METAL_SRCARG_STRING_

// special cases for comparison opcodes...

static const char *get_METAL_comparison_string_scalar(Context *ctx)
{
    static const char *comps[] = { "", ">", "==", ">=", "<", "!=", "<=" };
    if (ctx->instruction_controls >= STATICARRAYLEN(comps))
    {
        fail(ctx, "unknown comparison control");
        return "";
    } // if

    return comps[ctx->instruction_controls];
} // get_METAL_comparison_string_scalar

static const char *get_METAL_comparison_string_vector(Context *ctx)
{
    return get_METAL_comparison_string_scalar(ctx);  // standard C operators work for vectors in Metal.
} // get_METAL_comparison_string_vector


static void emit_METAL_start(Context *ctx, const char *profilestr)
{
    if (!shader_is_vertex(ctx) && !shader_is_pixel(ctx))
    {
        failf(ctx, "Shader type %u unsupported in this profile.",
              (uint) ctx->shader_type);
        return;
    } // if

    if (!ctx->mainfn)
    {
        if (shader_is_vertex(ctx))
            ctx->mainfn = StrDup(ctx, "VertexShader");
        else if (shader_is_pixel(ctx))
            ctx->mainfn = StrDup(ctx, "FragmentShader");
    } // if

    set_output(ctx, &ctx->mainline);
    ctx->indent++;
} // emit_METAL_start

static void emit_METAL_RET(Context *ctx);
static void emit_METAL_end(Context *ctx)
{
    // !!! FIXME: maybe handle this at a higher level?
    // ps_1_* writes color to r0 instead oC0. We move it to the right place.
    // We don't have to worry about a RET opcode messing this up, since
        //  RET isn't available before ps_2_0.
    if (shader_is_pixel(ctx) && !shader_version_atleast(ctx, 2, 0))
    {
        set_used_register(ctx, REG_TYPE_COLOROUT, 0, 1);
        output_line(ctx, "oC0 = r0;");
    } // if

    // !!! FIXME: maybe handle this at a higher level?
    // force a RET opcode if we're at the end of the stream without one.
    if (ctx->previous_opcode != OPCODE_RET)
        emit_METAL_RET(ctx);
} // emit_METAL_end

static void emit_METAL_phase(Context *ctx)
{
    // no-op in Metal.
} // emit_METAL_phase

static void emit_METAL_finalize(Context *ctx)
{
    // If we had a relative addressing of REG_TYPE_INPUT, we need to build
    //  an array for it at the start of main(). GLSL doesn't let you specify
    //  arrays of attributes.
    //float4 blah_array[BIGGEST_ARRAY];
    if (ctx->have_relative_input_registers) // !!! FIXME
        fail(ctx, "Relative addressing of input registers not supported.");

    // Insert header includes we need...
    push_output(ctx, &ctx->preflight);
    #define INC_METAL_HEADER(name) \
        if (ctx->metal_need_header_##name) { \
            output_line(ctx, "#include <metal_" #name ">"); \
        }
    INC_METAL_HEADER(common);
    INC_METAL_HEADER(math);
    INC_METAL_HEADER(relational);
    INC_METAL_HEADER(geometric);
    INC_METAL_HEADER(graphics);
    INC_METAL_HEADER(texture);
    #undef INC_METAL_HEADER
    output_blank_line(ctx);
    output_line(ctx, "using namespace metal;");
    output_blank_line(ctx);
    pop_output(ctx);

    // Fill in the shader's mainline function signature.
    push_output(ctx, &ctx->mainline_intro);
    output_line(ctx, "%s %s%s %s (",
                shader_is_vertex(ctx) ? "vertex" : "fragment",
                ctx->outputs ? ctx->mainfn : "void",
                ctx->outputs ? "_Output" : "", ctx->mainfn);
    pop_output(ctx);

    push_output(ctx, &ctx->mainline_arguments);
    ctx->indent++;

    const int uniform_count = ctx->uniform_float4_count + ctx->uniform_int4_count + ctx->uniform_bool_count;
    int commas = 0;
    if (uniform_count) commas++;
    if (ctx->inputs) commas++;
    if (commas) commas--;

    if (uniform_count > 0)
    {
        push_output(ctx, &ctx->globals);
        output_line(ctx, "struct %s_Uniforms", ctx->mainfn);
        output_line(ctx, "{");
        ctx->indent++;
        if (ctx->uniform_float4_count > 0)
            output_line(ctx, "float4 uniforms_float4[%d];", ctx->uniform_float4_count);
        if (ctx->uniform_int4_count > 0)
            output_line(ctx, "int4 uniforms_int4[%d];", ctx->uniform_int4_count);
        if (ctx->uniform_bool_count > 0)
            output_line(ctx, "bool uniforms_bool[%d];", ctx->uniform_bool_count);
        ctx->indent--;
        output_line(ctx, "};");
        pop_output(ctx);

        output_line(ctx, "constant %s_Uniforms &uniforms [[buffer(16)]]%s", ctx->mainfn, commas ? "," : "");
        commas--;
    } // if

    if (ctx->inputs)
    {
        output_line(ctx, "%s_Input input [[stage_in]]%s", ctx->mainfn, commas ? "," : "");
        commas--;
    } // if

    ctx->indent--;
    output_line(ctx, ") {");
    if (ctx->outputs)
    {
        ctx->indent++;
        output_line(ctx, "%s_Output output;", ctx->mainfn);

        push_output(ctx, &ctx->mainline);
        ctx->indent++;
        output_line(ctx, "return output;");
        pop_output(ctx);
    } // if
    pop_output(ctx);

    if (ctx->inputs)
    {
        push_output(ctx, &ctx->inputs);
        output_line(ctx, "};");
        output_blank_line(ctx);
        pop_output(ctx);
    } // if

    if (ctx->outputs)
    {
        push_output(ctx, &ctx->outputs);
        output_line(ctx, "};");
        output_blank_line(ctx);
        pop_output(ctx);
    } // if

    // throw some blank lines around to make source more readable.
    if (ctx->globals)  // don't add a blank line if the section is empty.
    {
        push_output(ctx, &ctx->globals);
        output_blank_line(ctx);
        pop_output(ctx);
    } // if
} // emit_METAL_finalize

static void emit_METAL_global(Context *ctx, RegisterType regtype, int regnum)
{
    char varname[64];
    get_METAL_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    // These aren't actually global in metal, set them up at top of mainline.
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;

    switch (regtype)
    {
        case REG_TYPE_ADDRESS:
            if (shader_is_vertex(ctx))
                output_line(ctx, "int4 %s;", varname);
            else if (shader_is_pixel(ctx))  // actually REG_TYPE_TEXTURE.
            {
                // We have to map texture registers to temps for ps_1_1, since
                //  they work like temps, initialize with tex coords, and the
                //  ps_1_1 TEX opcode expects to overwrite it.
                if (!shader_version_atleast(ctx, 1, 4))
                    output_line(ctx, "float4 %s = input.%s;",varname,varname);
            } // else if
            break;
        case REG_TYPE_PREDICATE:
            output_line(ctx, "bool4 %s;", varname);
            break;
        case REG_TYPE_TEMP:
            output_line(ctx, "float4 %s;", varname);
            break;
        case REG_TYPE_LOOP:
            break; // no-op. We declare these in for loops at the moment.
        case REG_TYPE_LABEL:
            break; // no-op. If we see it here, it means we optimized it out.
        default:
            fail(ctx, "BUG: we used a register we don't know how to define.");
            break;
    } // switch

    pop_output(ctx);
} // emit_METAL_global

static void emit_METAL_array(Context *ctx, VariableList *var)
{
    // All uniforms (except constant arrays, which are literally constant
    //  data embedded in Metal shaders) are now packed into a single array,
    //  so we can batch the uniform transfers. So this doesn't actually
    //  define an array here; the one, big array is emitted during
    //  finalization instead.
    // However, we need to #define the offset into the one, big array here,
    //  and let dereferences use that #define.
    const int base = var->index;
    const int metalbase = ctx->uniform_float4_count;
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const int ARRAYBASE_%d = %d;", base, metalbase);
    pop_output(ctx);
    var->emit_position = metalbase;
} // emit_METAL_array

static void emit_METAL_const_array(Context *ctx, const ConstantsList *clist,
                                   int base, int size)
{
    char varname[64];
    get_METAL_const_array_varname_in_buf(ctx,base,size,varname,sizeof(varname));

    const char *cstr = NULL;
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const float4 %s[%d] = {", varname, size);
    ctx->indent++;

    int i;
    for (i = 0; i < size; i++)
    {
        while (clist->constant.type != MOJOSHADER_UNIFORM_FLOAT)
            clist = clist->next;
        assert(clist->constant.index == (base + i));

        char val0[32];
        char val1[32];
        char val2[32];
        char val3[32];
        floatstr(ctx, val0, sizeof (val0), clist->constant.value.f[0], 1);
        floatstr(ctx, val1, sizeof (val1), clist->constant.value.f[1], 1);
        floatstr(ctx, val2, sizeof (val2), clist->constant.value.f[2], 1);
        floatstr(ctx, val3, sizeof (val3), clist->constant.value.f[3], 1);

        output_line(ctx, "float4(%s, %s, %s, %s)%s", val0, val1, val2, val3,
                        (i < (size-1)) ? "," : "");

        clist = clist->next;
    } // for

    ctx->indent--;
    output_line(ctx, "};");
    output_line(ctx, "(void) %s[0];", varname);  // stop compiler warnings.
    pop_output(ctx);
} // emit_METAL_const_array

static void emit_METAL_uniform(Context *ctx, RegisterType regtype, int regnum,
                              const VariableList *var)
{
    // Now that we're pushing all the uniforms as one struct, pack these
    //  down, so if we only use register c439, it'll actually map to
    //  uniforms.uniforms_float4[0]. As we push one big struct, this will
    //  prevent uploading unused data.

    const char *utype = get_METAL_uniform_type(ctx, regtype);
    char varname[64];
    char name[64];
    int index = 0;

    get_METAL_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;

    if (var == NULL)
    {
        get_METAL_uniform_array_varname(ctx, regtype, name, sizeof (name));

        if (regtype == REG_TYPE_CONST)
            index = ctx->uniform_float4_count;
        else if (regtype == REG_TYPE_CONSTINT)
            index = ctx->uniform_int4_count;
        else if (regtype == REG_TYPE_CONSTBOOL)
            index = ctx->uniform_bool_count;
        else  // get_METAL_uniform_array_varname() would have called fail().
            assert(isfail(ctx));

        // !!! FIXME: can cause unused var warnings in Clang...
        //output_line(ctx, "constant %s &%s = %s[%d];", utype, varname, name, index);
        output_line(ctx, "#define %s %s[%d]", varname, name, index);
        push_output(ctx, &ctx->mainline);
        ctx->indent++;
        output_line(ctx, "#undef %s", varname);  // !!! FIXME: gross.
        pop_output(ctx);
    } // if

    else
    {
        const int arraybase = var->index;
        if (var->constant)
        {
            get_METAL_const_array_varname_in_buf(ctx, arraybase, var->count,
                                                name, sizeof (name));
            index = (regnum - arraybase);
        } // if
        else
        {
            assert(var->emit_position != -1);
            get_METAL_uniform_array_varname(ctx, regtype, name, sizeof (name));
            index = (regnum - arraybase) + var->emit_position;
        } // else

        // !!! FIXME: might trigger unused var warnings in Clang.
        //output_line(ctx, "constant %s &%s = %s[%d];", utype, varname, name, index);
        output_line(ctx, "#define %s %s[%d];", varname, name, index);
        push_output(ctx, &ctx->mainline);
        ctx->indent++;
        output_line(ctx, "#undef %s", varname);  // !!! FIXME: gross.
        pop_output(ctx);
    } // else

    pop_output(ctx);
} // emit_METAL_uniform

static void emit_METAL_sampler(Context *ctx,int stage,TextureType ttype,int tb)
{
    char var[64];
    const char *texsuffix = NULL;
    switch (ttype)
    {
        case TEXTURE_TYPE_2D: texsuffix = "2d"; break;
        case TEXTURE_TYPE_CUBE: texsuffix = "cube"; break;
        case TEXTURE_TYPE_VOLUME: texsuffix = "3d"; break;
        default: assert(!"unexpected texture type"); return;
    } // switch

    get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, stage, var, sizeof (var));

    push_output(ctx, &ctx->mainline_arguments);
    ctx->indent++;
    output_line(ctx, "texture%s<float> %s_texture [[texture(%d)]],",
                texsuffix, var, stage);
    output_line(ctx, "sampler %s [[sampler(%d)]],", var, stage);
    pop_output(ctx);

    if (tb)  // This sampler used a ps_1_1 TEXBEM opcode?
    {
        push_output(ctx, &ctx->mainline_top);
        ctx->indent++;
        char name[64];
        const int index = ctx->uniform_float4_count;
        ctx->uniform_float4_count += 2;
        get_METAL_uniform_array_varname(ctx, REG_TYPE_CONST, name, sizeof (name));
        output_line(ctx, "constant float4 &%s_texbem = %s[%d];", var, name, index);
        output_line(ctx, "constant float4 &%s_texbeml = %s[%d];", var, name, index+1);
        pop_output(ctx);
    } // if
} // emit_METAL_sampler

static void emit_METAL_attribute(Context *ctx, RegisterType regtype, int regnum,
                                MOJOSHADER_usage usage, int index, int wmask,
                                int flags)
{
    // !!! FIXME: this function doesn't deal with write masks at all yet!
    const char *usage_str = NULL;
    char index_str[16] = { '\0' };
    char var[64];

    get_METAL_varname_in_buf(ctx, regtype, regnum, var, sizeof (var));

    //assert((flags & MOD_PP) == 0);  // !!! FIXME: is PP allowed?

    if (index != 0)  // !!! FIXME: a lot of these MUST be zero.
        snprintf(index_str, sizeof (index_str), "%u", (uint) index);

    if (shader_is_vertex(ctx))
    {
        // pre-vs3 output registers.
        // these don't ever happen in DCL opcodes, I think. Map to vs_3_*
        //  output registers.
        if (!shader_version_atleast(ctx, 3, 0))
        {
            if (regtype == REG_TYPE_RASTOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                index = regnum;
                switch ((const RastOutType) regnum)
                {
                    case RASTOUT_TYPE_POSITION:
                        usage = MOJOSHADER_USAGE_POSITION;
                        break;
                    case RASTOUT_TYPE_FOG:
                        usage = MOJOSHADER_USAGE_FOG;
                        break;
                    case RASTOUT_TYPE_POINT_SIZE:
                        usage = MOJOSHADER_USAGE_POINTSIZE;
                        break;
                } // switch
            } // if

            else if (regtype == REG_TYPE_ATTROUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_COLOR;
                index = regnum;
            } // else if

            else if (regtype == REG_TYPE_TEXCRDOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_TEXCOORD;
                index = regnum;
            } // else if
        } // if

        if (regtype == REG_TYPE_INPUT)
        {
            push_output(ctx, &ctx->inputs);
            if (buffer_size(ctx->inputs) == 0)
            {
                output_line(ctx, "struct %s_Input", ctx->mainfn);
                output_line(ctx, "{");
            } // if

            ctx->indent++;
            output_line(ctx, "float4 %s [[attribute(%d)]];", var, regnum);
            pop_output(ctx);

            push_output(ctx, &ctx->mainline_top);
            ctx->indent++;
            // !!! FIXME: might trigger unused var warnings in Clang.
            //output_line(ctx, "constant float4 &%s = input.%s;", var, var);
            output_line(ctx, "#define %s input.%s", var, var);
            pop_output(ctx);
            push_output(ctx, &ctx->mainline);
            ctx->indent++;
            output_line(ctx, "#undef %s", var);  // !!! FIXME: gross.
            pop_output(ctx);
        } // if

        else if (regtype == REG_TYPE_OUTPUT)
        {
            push_output(ctx, &ctx->outputs);
            if (buffer_size(ctx->outputs) == 0)
            {
                output_line(ctx, "struct %s_Output", ctx->mainfn);
                output_line(ctx, "{");
            } // if

            ctx->indent++;

            switch (usage)
            {
                case MOJOSHADER_USAGE_POSITION:
                    output_line(ctx, "float4 %s [[position]];", var);
                    break;
                case MOJOSHADER_USAGE_POINTSIZE:
                    output_line(ctx, "float4 %s [[point_size]];", var);
                    break;
                case MOJOSHADER_USAGE_COLOR:
                    output_line(ctx, "float4 %s [[user(color%d)]];", var, index);
                    break;
                case MOJOSHADER_USAGE_FOG:
                    output_line(ctx, "float4 %s [[user(fog)]];", var);
                    break;
                case MOJOSHADER_USAGE_TEXCOORD:
                    output_line(ctx, "float4 %s [[user(texcoord%d)]];", var, index);
                    break;
                default:
                    // !!! FIXME: we need to deal with some more built-in varyings here.
                    break;
            } // switch

            pop_output(ctx);

            push_output(ctx, &ctx->mainline_top);
            ctx->indent++;
            // !!! FIXME: this doesn't work.
            //output_line(ctx, "float4 &%s = output.%s;", var, var);
            output_line(ctx, "#define %s output.%s", var, var);
            pop_output(ctx);
            push_output(ctx, &ctx->mainline);
            ctx->indent++;
            output_line(ctx, "#undef %s", var);  // !!! FIXME: gross.
            pop_output(ctx);
        } // else if

        else
        {
            fail(ctx, "unknown vertex shader attribute register");
        } // else
    } // if

    else if (shader_is_pixel(ctx))
    {
        // samplers DCLs get handled in emit_METAL_sampler().

        if (flags & MOD_CENTROID)  // !!! FIXME
        {
            failf(ctx, "centroid unsupported in %s profile", ctx->profile->name);
            return;
        } // if

        if ((regtype == REG_TYPE_COLOROUT) || (regtype == REG_TYPE_DEPTHOUT))
        {
            push_output(ctx, &ctx->outputs);
            if (buffer_size(ctx->outputs) == 0)
            {
                output_line(ctx, "struct %s_Output", ctx->mainfn);
                output_line(ctx, "{");
            } // if
            ctx->indent++;

            if (regtype == REG_TYPE_COLOROUT)
                output_line(ctx, "float4 %s [[color(%d)]];", var, regnum);
            else if (regtype == REG_TYPE_DEPTHOUT)
                output_line(ctx, "float %s [[depth(any)]];", var);

            pop_output(ctx);

            push_output(ctx, &ctx->mainline_top);
            ctx->indent++;
            // !!! FIXME: this doesn't work.
            //output_line(ctx, "float%s &%s = output.%s;", (regtype == REG_TYPE_DEPTHOUT) ? "" : "4", var, var);
            output_line(ctx, "#define %s output.%s", var, var);
            pop_output(ctx);
            push_output(ctx, &ctx->mainline);
            ctx->indent++;
            output_line(ctx, "#undef %s", var);  // !!! FIXME: gross.
            pop_output(ctx);
        } // if

        // !!! FIXME: can you actualy have a texture register with COLOR usage?
        else if ((regtype == REG_TYPE_TEXTURE) ||
                 (regtype == REG_TYPE_INPUT) ||
                 (regtype == REG_TYPE_MISCTYPE))
        {
            int skipreference = 0;
            push_output(ctx, &ctx->inputs);
            if (buffer_size(ctx->inputs) == 0)
            {
                output_line(ctx, "struct %s_Input", ctx->mainfn);
                output_line(ctx, "{");
            } // if
            ctx->indent++;

            if (regtype == REG_TYPE_MISCTYPE)
            {
                const MiscTypeType mt = (MiscTypeType) regnum;
                if (mt == MISCTYPE_TYPE_FACE)
                    output_line(ctx, "bool %s [[front_facing]];", var);
                else if (mt == MISCTYPE_TYPE_POSITION)
                    output_line(ctx, "float4 %s [[position]];", var);
                else
                    fail(ctx, "BUG: unhandled misc register");
            } // else if

            else
            {
                if (usage == MOJOSHADER_USAGE_TEXCOORD)
                {
                    // ps_1_1 does a different hack for this attribute.
                    //  Refer to emit_METAL_global()'s REG_TYPE_ADDRESS code.
                    if (!shader_version_atleast(ctx, 1, 4))
                        skipreference = 1;
                    output_line(ctx, "float4 %s [[user(texcoord%d)]];", var, index);
                } // if

                else if (usage == MOJOSHADER_USAGE_COLOR)
                    output_line(ctx, "float4 %s [[user(color%d)]];", var, index);

                else if (usage == MOJOSHADER_USAGE_FOG)
                    output_line(ctx, "float4 %s [[user(fog)]];", var);
            } // else

            pop_output(ctx);

            // !!! FIXME: can cause unused var warnings in Clang...
            #if 0
            push_output(ctx, &ctx->mainline_top);
            ctx->indent++;
            if ((regtype == REG_TYPE_MISCTYPE)&&(regnum == MISCTYPE_TYPE_FACE))
                output_line(ctx, "constant bool &%s = input.%s;", var, var);
            else if (!skipreference)
                output_line(ctx, "constant float4 &%s = input.%s;", var, var);
            pop_output(ctx);
            #endif

            if (!skipreference)
            {
                push_output(ctx, &ctx->mainline_top);
                ctx->indent++;
                output_line(ctx, "#define %s input.%s", var, var);
                pop_output(ctx);
                push_output(ctx, &ctx->mainline);
                ctx->indent++;
                output_line(ctx, "#undef %s", var);  // !!! FIXME: gross.
                pop_output(ctx);
            } // if
        } // else if

        else
        {
            fail(ctx, "unknown pixel shader attribute register");
        } // else
    } // else if

    else
    {
        fail(ctx, "Unknown shader type");  // state machine should catch this.
    } // else
} // emit_METAL_attribute

static void emit_METAL_NOP(Context *ctx)
{
    // no-op is a no-op.  :)
} // emit_METAL_NOP

static void emit_METAL_MOV(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_METAL_destarg_assign(ctx, code, sizeof (code), "%s", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_MOV

static void emit_METAL_ADD(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_METAL_destarg_assign(ctx, code, sizeof (code), "%s + %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_ADD

static void emit_METAL_SUB(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_METAL_destarg_assign(ctx, code, sizeof (code), "%s - %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_SUB

static void emit_METAL_MAD(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_METAL_srcarg_string_masked(ctx, 2, src2, sizeof (src2));
    char code[128];
    make_METAL_destarg_assign(ctx, code, sizeof (code), "(%s * %s) + %s", src0, src1, src2);
    output_line(ctx, "%s", code);
} // emit_METAL_MAD

static void emit_METAL_MUL(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    make_METAL_destarg_assign(ctx, code, sizeof (code), "%s * %s", src0, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_MUL

static void emit_METAL_RCP(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    make_METAL_destarg_assign(ctx, code, sizeof (code), "1.0 / %s", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_RCP

static void emit_METAL_RSQ(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "rsqrt(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_RSQ

static void emit_METAL_dotprod(Context *ctx, const char *src0, const char *src1,
                              const char *extra)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char castleft[16] = { '\0' };
    const char *castright = "";
    if (vecsize != 1)
    {
        snprintf(castleft, sizeof (castleft), "float%d(", vecsize);
        castright = ")";
    } // if

    char code[128];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "%sdot(%s, %s)%s%s",
                             castleft, src0, src1, extra, castright);
    output_line(ctx, "%s", code);
} // emit_METAL_dotprod

static void emit_METAL_DP3(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_vec3(ctx, 1, src1, sizeof (src1));
    emit_METAL_dotprod(ctx, src0, src1, "");
} // emit_METAL_DP3

static void emit_METAL_DP4(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_full(ctx, 1, src1, sizeof (src1));
    emit_METAL_dotprod(ctx, src0, src1, "");
} // emit_METAL_DP4

static void emit_METAL_MIN(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "min(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_MIN

static void emit_METAL_MAX(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "max(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_MAX

static void emit_METAL_SLT(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];

    // float(bool) or vec(bvec) results in 0.0 or 1.0, like SLT wants.
    if (vecsize == 1)
        make_METAL_destarg_assign(ctx, code, sizeof (code), "float(%s < %s)", src0, src1);
    else
    {
        make_METAL_destarg_assign(ctx, code, sizeof (code),
                                  "float%d(%s < %s)", vecsize, src0, src1);
    } // else
    output_line(ctx, "%s", code);
} // emit_METAL_SLT

static void emit_METAL_SGE(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];

    // float(bool) or vec(bvec) results in 0.0 or 1.0, like SGE wants.
    if (vecsize == 1)
    {
        make_METAL_destarg_assign(ctx, code, sizeof (code),
                                 "float(%s >= %s)", src0, src1);
    } // if
    else
    {
        make_METAL_destarg_assign(ctx, code, sizeof (code),
                                  "float%d(%s >= %s)", vecsize, src0, src1);
    } // else
    output_line(ctx, "%s", code);
} // emit_METAL_SGE

static void emit_METAL_EXP(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "exp2(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_EXP

static void emit_METAL_LOG(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "log2(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_LOG

static void emit_METAL_LIT_helper(Context *ctx)
{
    const char *maxp = "127.9961"; // value from the dx9 reference.

    if (ctx->glsl_generated_lit_helper)
        return;

    ctx->glsl_generated_lit_helper = 1;
    ctx->metal_need_header_common = 1;
    ctx->metal_need_header_math = 1;

    push_output(ctx, &ctx->helpers);
    output_line(ctx, "static float4 LIT(const float4 src)");
    output_line(ctx, "{"); ctx->indent++;
    output_line(ctx,   "const float power = clamp(src.w, -%s, %s);",maxp,maxp);
    output_line(ctx,   "float4 retval = float4(1.0, 0.0, 0.0, 1.0);");
    output_line(ctx,   "if (src.x > 0.0) {"); ctx->indent++;
    output_line(ctx,     "retval.y = src.x;");
    output_line(ctx,     "if (src.y > 0.0) {"); ctx->indent++;
    output_line(ctx,       "retval.z = pow(src.y, power);"); ctx->indent--;
    output_line(ctx,     "}"); ctx->indent--;
    output_line(ctx,   "}");
    output_line(ctx,   "return retval;"); ctx->indent--;
    output_line(ctx, "}");
    output_blank_line(ctx);
    pop_output(ctx);
} // emit_METAL_LIT_helper

static void emit_METAL_LIT(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char code[128];
    emit_METAL_LIT_helper(ctx);
    make_METAL_destarg_assign(ctx, code, sizeof (code), "LIT(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_LIT

static void emit_METAL_DST(Context *ctx)
{
    // !!! FIXME: needs to take ctx->dst_arg.writemask into account.
    char src0_y[64]; make_METAL_srcarg_string_y(ctx, 0, src0_y, sizeof (src0_y));
    char src1_y[64]; make_METAL_srcarg_string_y(ctx, 1, src1_y, sizeof (src1_y));
    char src0_z[64]; make_METAL_srcarg_string_z(ctx, 0, src0_z, sizeof (src0_z));
    char src1_w[64]; make_METAL_srcarg_string_w(ctx, 1, src1_w, sizeof (src1_w));

    char code[128];
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                             "float4(1.0, %s * %s, %s, %s)",
                             src0_y, src1_y, src0_z, src1_w);
    output_line(ctx, "%s", code);
} // emit_METAL_DST

static void emit_METAL_LRP(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_METAL_srcarg_string_masked(ctx, 2, src2, sizeof (src2));
    char code[128];
    ctx->metal_need_header_common = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "mix(%s, %s, %s)",
                             src2, src1, src0);
    output_line(ctx, "%s", code);
} // emit_METAL_LRP

static void emit_METAL_FRC(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "fract(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_FRC

static void emit_METAL_M4X4(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_METAL_srcarg_string_full(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_METAL_srcarg_string_full(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_METAL_srcarg_string_full(ctx, 3, row2, sizeof (row2));
    char row3[64]; make_METAL_srcarg_string_full(ctx, 4, row3, sizeof (row3));
    char code[256];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                    "float4(dot(%s, %s), dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                    src0, row0, src0, row1, src0, row2, src0, row3);
    output_line(ctx, "%s", code);
} // emit_METAL_M4X4

static void emit_METAL_M4X3(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_full(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_METAL_srcarg_string_full(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_METAL_srcarg_string_full(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_METAL_srcarg_string_full(ctx, 3, row2, sizeof (row2));
    char code[256];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                                "float3(dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1, src0, row2);
    output_line(ctx, "%s", code);
} // emit_METAL_M4X3

static void emit_METAL_M3X4(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_METAL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_METAL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_METAL_srcarg_string_vec3(ctx, 3, row2, sizeof (row2));
    char row3[64]; make_METAL_srcarg_string_vec3(ctx, 4, row3, sizeof (row3));
    char code[256];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                                "float4(dot(%s, %s), dot(%s, %s), "
                                     "dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1,
                                src0, row2, src0, row3);
    output_line(ctx, "%s", code);
} // emit_METAL_M3X4

static void emit_METAL_M3X3(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_METAL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_METAL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char row2[64]; make_METAL_srcarg_string_vec3(ctx, 3, row2, sizeof (row2));
    char code[256];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                                "float3(dot(%s, %s), dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1, src0, row2);
    output_line(ctx, "%s", code);
} // emit_METAL_M3X3

static void emit_METAL_M3X2(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char row0[64]; make_METAL_srcarg_string_vec3(ctx, 1, row0, sizeof (row0));
    char row1[64]; make_METAL_srcarg_string_vec3(ctx, 2, row1, sizeof (row1));
    char code[256];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                                "float2(dot(%s, %s), dot(%s, %s))",
                                src0, row0, src0, row1);
    output_line(ctx, "%s", code);
} // emit_METAL_M3X2

static void emit_METAL_CALL(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    if (ctx->loops > 0)
        output_line(ctx, "%s(aL);", src0);
    else
        output_line(ctx, "%s();", src0);
} // emit_METAL_CALL

static void emit_METAL_CALLNZ(Context *ctx)
{
    // !!! FIXME: if src1 is a constbool that's true, we can remove the
    // !!! FIXME:  if. If it's false, we can make this a no-op.
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));

    if (ctx->loops > 0)
        output_line(ctx, "if (%s) { %s(aL); }", src1, src0);
    else
        output_line(ctx, "if (%s) { %s(); }", src1, src0);
} // emit_METAL_CALLNZ

static void emit_METAL_LOOP(Context *ctx)
{
    // !!! FIXME: swizzle?
    char var[64]; get_METAL_srcarg_varname(ctx, 1, var, sizeof (var));
    assert(ctx->source_args[0].regnum == 0);  // in case they add aL1 someday.
    output_line(ctx, "{");
    ctx->indent++;
    output_line(ctx, "const int aLend = %s.x + %s.y;", var, var);
    output_line(ctx, "for (int aL = %s.y; aL < aLend; aL += %s.z) {", var, var);
    ctx->indent++;
} // emit_METAL_LOOP

static void emit_METAL_RET(Context *ctx)
{
    // thankfully, the MSDN specs say a RET _has_ to end a function...no
    //  early returns. So if you hit one, you know you can safely close
    //  a high-level function.
    push_output(ctx, &ctx->postflight);
    output_line(ctx, "}");
    output_blank_line(ctx);
    set_output(ctx, &ctx->subroutines);  // !!! FIXME: is this for LABEL? Maybe set it there so we don't allocate unnecessarily.
} // emit_METAL_RET

static void emit_METAL_ENDLOOP(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
    ctx->indent--;
    output_line(ctx, "}");
} // emit_METAL_ENDLOOP

static void emit_METAL_LABEL(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    const int label = ctx->source_args[0].regnum;
    RegisterList *reg = reglist_find(&ctx->used_registers, REG_TYPE_LABEL, label);
    assert(ctx->output == ctx->subroutines);  // not mainline, etc.
    assert(ctx->indent == 0);  // we shouldn't be in the middle of a function.

    // MSDN specs say CALL* has to come before the LABEL, so we know if we
    //  can ditch the entire function here as unused.
    if (reg == NULL)
        set_output(ctx, &ctx->ignore);  // Func not used. Parse, but don't output.

    // !!! FIXME: it would be nice if we could determine if a function is
    // !!! FIXME:  only called once and, if so, forcibly inline it.

    // !!! FIXME: this worked in GLSL because all our state is global to the shader,
    // !!! FIXME:  but in metal we kept it local to the shader mainline.
    // !!! FIXME:  Can we do C++11 lambdas in Metal to have nested functions?  :)

    const char *uses_loopreg = ((reg) && (reg->misc == 1)) ? "int aL" : "";
    output_line(ctx, "static void %s(%s)", src0, uses_loopreg);
    output_line(ctx, "{");
    ctx->indent++;
} // emit_METAL_LABEL

static void emit_METAL_DCL(Context *ctx)
{
    // no-op. We do this in our emit_attribute() and emit_uniform().
} // emit_METAL_DCL

static void emit_METAL_POW(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                             "pow(abs(%s), %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_POW

static void emit_METAL_CRS(Context *ctx)
{
    // !!! FIXME: needs to take ctx->dst_arg.writemask into account.
    char src0[64]; make_METAL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_vec3(ctx, 1, src1, sizeof (src1));
    char code[128];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code),
                             "cross(%s, %s)", src0, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_CRS

static void emit_METAL_SGN(Context *ctx)
{
    // (we don't need the temporary registers specified for the D3D opcode.)
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_common = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "sign(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_SGN

static void emit_METAL_ABS(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_math = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "abs(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_ABS

static void emit_METAL_NRM(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_geometric = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "normalize(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_NRM

static void emit_METAL_SINCOS(Context *ctx)
{
    // we don't care about the temp registers that <= sm2 demands; ignore them.
    //  sm2 also talks about what components are left untouched vs. undefined,
    //  but we just leave those all untouched with Metal write masks (which
    //  would fulfill the "undefined" requirement, too).
    const int mask = ctx->dest_arg.writemask;
    char src0[64]; make_METAL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char code[128] = { '\0' };

    ctx->metal_need_header_math = 1;
    if (writemask_x(mask))
        make_METAL_destarg_assign(ctx, code, sizeof (code), "cos(%s)", src0);
    else if (writemask_y(mask))
        make_METAL_destarg_assign(ctx, code, sizeof (code), "sin(%s)", src0);
    else if (writemask_xy(mask))
    {
        // !!! FIXME: can use sincos(), but need to assign cos to a temp, since it needs a reference.
        make_METAL_destarg_assign(ctx, code, sizeof (code),
                                 "float2(cos(%s), sin(%s))", src0, src0);
    } // else if

    output_line(ctx, "%s", code);
} // emit_METAL_SINCOS

static void emit_METAL_REP(Context *ctx)
{
    // !!! FIXME:
    // msdn docs say legal loop values are 0 to 255. We can check DEFI values
    //  at parse time, but if they are pulling a value from a uniform, do
    //  we clamp here?
    // !!! FIXME: swizzle is legal here, right?
    char src0[64]; make_METAL_srcarg_string_x(ctx, 0, src0, sizeof (src0));
    const uint rep = (uint) ctx->reps;
    output_line(ctx, "for (int rep%u = 0; rep%u < %s; rep%u++) {",
                rep, rep, src0, rep);
    ctx->indent++;
} // emit_METAL_REP

static void emit_METAL_ENDREP(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
} // emit_METAL_ENDREP

static void emit_METAL_IF(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    output_line(ctx, "if (%s) {", src0);
    ctx->indent++;
} // emit_METAL_IF

static void emit_METAL_IFC(Context *ctx)
{
    const char *comp = get_METAL_comparison_string_scalar(ctx);
    char src0[64]; make_METAL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "if (%s %s %s) {", src0, comp, src1);
    ctx->indent++;
} // emit_METAL_IFC

static void emit_METAL_ELSE(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "} else {");
    ctx->indent++;
} // emit_METAL_ELSE

static void emit_METAL_ENDIF(Context *ctx)
{
    ctx->indent--;
    output_line(ctx, "}");
} // emit_METAL_ENDIF

static void emit_METAL_BREAK(Context *ctx)
{
    output_line(ctx, "break;");
} // emit_METAL_BREAK

static void emit_METAL_BREAKC(Context *ctx)
{
    const char *comp = get_METAL_comparison_string_scalar(ctx);
    char src0[64]; make_METAL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_scalar(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "if (%s %s %s) { break; }", src0, comp, src1);
} // emit_METAL_BREAKC

static void emit_METAL_MOVA(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];

    ctx->metal_need_header_math = 1;
    ctx->metal_need_header_common = 1;

    if (vecsize == 1)
    {
        make_METAL_destarg_assign(ctx, code, sizeof (code),
                                 "int(floor(abs(%s) + 0.5) * sign(%s))",
                                 src0, src0);
    } // if

    else
    {
        make_METAL_destarg_assign(ctx, code, sizeof (code),
                            "int%d(floor(abs(%s) + float%d(0.5)) * sign(%s))",
                            vecsize, src0, vecsize, src0);
    } // else

    output_line(ctx, "%s", code);
} // emit_METAL_MOVA

static void emit_METAL_DEFB(Context *ctx)
{
    char varname[64]; get_METAL_destarg_varname(ctx, varname, sizeof (varname));
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const bool %s = %s;",
                varname, ctx->dwords[0] ? "true" : "false");
    pop_output(ctx);
} // emit_METAL_DEFB

static void emit_METAL_DEFI(Context *ctx)
{
    char varname[64]; get_METAL_destarg_varname(ctx, varname, sizeof (varname));
    const int32 *x = (const int32 *) ctx->dwords;
    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    output_line(ctx, "const int4 %s = int4(%d, %d, %d, %d);",
                varname, (int) x[0], (int) x[1], (int) x[2], (int) x[3]);
    pop_output(ctx);
} // emit_METAL_DEFI

EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXCRD)

static void emit_METAL_TEXKILL(Context *ctx)
{
    char dst[64]; get_METAL_destarg_varname(ctx, dst, sizeof (dst));
    ctx->metal_need_header_relational = 1;
    ctx->metal_need_header_graphics = 1;
    output_line(ctx, "if (any(%s.xyz < float3(0.0))) discard_fragment();", dst);
} // emit_METAL_TEXKILL

static void metal_texld(Context *ctx, const int texldd)
{
    ctx->metal_need_header_texture = 1;
    if (!shader_version_atleast(ctx, 1, 4))
    {
        DestArgInfo *info = &ctx->dest_arg;
        char dst[64];
        char sampler[64];
        char code[128] = {0};

        assert(!texldd);

        RegisterList *sreg;
        sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER, info->regnum);
        const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);

        char swizzle[4] = { 'x', 'y', 'z', '\0' };
        if (ttype == TEXTURE_TYPE_2D)
            swizzle[2] = '\0';  // "xy" instead of "xyz".

        // !!! FIXME: this code counts on the register not having swizzles, etc.
        get_METAL_destarg_varname(ctx, dst, sizeof (dst));
        get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                                 sampler, sizeof (sampler));

        make_METAL_destarg_assign(ctx, code, sizeof (code),
                                  "%s_texture.sample(%s, %s.%s)",
                                  sampler, sampler, dst, swizzle);
        output_line(ctx, "%s", code);
    } // if

    else if (!shader_version_atleast(ctx, 2, 0))
    {
        // ps_1_4 is different, too!
        fail(ctx, "TEXLD == Shader Model 1.4 unimplemented.");  // !!! FIXME
        return;
    } // else if

    else
    {
        const SourceArgInfo *samp_arg = &ctx->source_args[1];
        RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                          samp_arg->regnum);
        const char *funcname = NULL;
        char src0[64] = { '\0' };
        char src1[64]; get_METAL_srcarg_varname(ctx, 1, src1, sizeof (src1)); // !!! FIXME: SRC_MOD?
        char src2[64] = { '\0' };
        char src3[64] = { '\0' };

        if (sreg == NULL)
        {
            fail(ctx, "TEXLD using undeclared sampler");
            return;
        } // if

        const char *grad = "";
        if (texldd)
        {
            switch ((const TextureType) sreg->index)
            {
                case TEXTURE_TYPE_2D:
                    grad = "2d";
                    make_METAL_srcarg_string_vec2(ctx, 2, src2, sizeof (src2));
                    make_METAL_srcarg_string_vec2(ctx, 3, src3, sizeof (src3));
                    break;
                case TEXTURE_TYPE_VOLUME:
                    grad = "3d";
                    make_METAL_srcarg_string_vec3(ctx, 2, src2, sizeof (src2));
                    make_METAL_srcarg_string_vec3(ctx, 3, src3, sizeof (src3));
                    break;
                case TEXTURE_TYPE_CUBE:
                    grad = "cube";
                    make_METAL_srcarg_string_vec3(ctx, 2, src2, sizeof (src2));
                    make_METAL_srcarg_string_vec3(ctx, 3, src3, sizeof (src3));
                    break;
            } // switch
        } // if

        // !!! FIXME: can TEXLDD set instruction_controls?
        // !!! FIXME: does the d3d bias value map directly to Metal?
        const char *biasleft = "";
        const char *biasright = "";
        char bias[64] = { '\0' };
        if (ctx->instruction_controls == CONTROL_TEXLDB)
        {
            biasleft = ", bias(";
            make_METAL_srcarg_string_w(ctx, 0, bias, sizeof (bias));
            biasright = ")";
        } // if

        // Metal doesn't have a texture2DProj() function, but you just divide
        // your texcoords by texcoords.w to achieve it anyhow, so DIY.
        const char *projop = "";
        char proj[64] = { '\0' };
        if (ctx->instruction_controls == CONTROL_TEXLDP)
        {
            if (sreg->index == TEXTURE_TYPE_CUBE)
                fail(ctx, "TEXLDP on a cubemap");  // !!! FIXME: is this legal?
            projop = " / ";
            make_METAL_srcarg_string_w(ctx, 0, proj, sizeof (proj));
        } // if

        switch ((const TextureType) sreg->index)
        {
            case TEXTURE_TYPE_2D:
                make_METAL_srcarg_string_vec2(ctx, 0, src0, sizeof (src0));
                break;

            case TEXTURE_TYPE_CUBE:
            case TEXTURE_TYPE_VOLUME:
                make_METAL_srcarg_string_vec3(ctx, 0, src0, sizeof (src0));
                break;

            default:
                fail(ctx, "unknown texture type");
                return;
        } // switch

        assert(!isscalar(ctx, ctx->shader_type, samp_arg->regtype, samp_arg->regnum));
        char swiz_str[6] = { '\0' };
        make_METAL_swizzle_string(swiz_str, sizeof (swiz_str),
                                 samp_arg->swizzle, ctx->dest_arg.writemask);

        char code[128];
        if (texldd)
        {
            make_METAL_destarg_assign(ctx, code, sizeof (code),
                                     "%s_texture.sample(%s, %s, gradient%s(%s, %s))%s",
                                     src1, src1, src0, grad, src2, src3, swiz_str);
        } // if
        else
        {
            make_METAL_destarg_assign(ctx, code, sizeof (code),
                                     "%s_texture.sample(%s, %s%s%s%s%s%s)%s",
                                     src1, src1, src0, projop, proj,
                                     biasleft, bias, biasright, swiz_str);
        } // else

        output_line(ctx, "%s", code);
    } // else
} // metal_texld

static void emit_METAL_TEXLD(Context *ctx)
{
    metal_texld(ctx, 0);
} // emit_METAL_TEXLD
    

static void emit_METAL_TEXBEM(Context *ctx)
{
    DestArgInfo *info = &ctx->dest_arg;
    char dst[64]; get_METAL_destarg_varname(ctx, dst, sizeof (dst));
    char src[64]; get_METAL_srcarg_varname(ctx, 0, src, sizeof (src));
    char sampler[64];
    char code[512];

    ctx->metal_need_header_texture = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    make_METAL_destarg_assign(ctx, code, sizeof (code),
        "%s_texture.sample(%s, float2(%s.x + (%s_texbem.x * %s.x) + (%s_texbem.z * %s.y),"
        " %s.y + (%s_texbem.y * %s.x) + (%s_texbem.w * %s.y)))",
        sampler, sampler,
        dst, sampler, src, sampler, src,
        dst, sampler, src, sampler, src);

    output_line(ctx, "%s", code);
} // emit_METAL_TEXBEM


static void emit_METAL_TEXBEML(Context *ctx)
{
    // !!! FIXME: this code counts on the register not having swizzles, etc.
    DestArgInfo *info = &ctx->dest_arg;
    char dst[64]; get_METAL_destarg_varname(ctx, dst, sizeof (dst));
    char src[64]; get_METAL_srcarg_varname(ctx, 0, src, sizeof (src));
    char sampler[64];
    char code[512];

    ctx->metal_need_header_texture = 1;

    get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    make_METAL_destarg_assign(ctx, code, sizeof (code),
        "(%s_texture.sample(%s, float2(%s.x + (%s_texbem.x * %s.x) + (%s_texbem.z * %s.y),"
        " %s.y + (%s_texbem.y * %s.x) + (%s_texbem.w * %s.y)))) *"
        " ((%s.z * %s_texbeml.x) + %s_texbem.y)",
        sampler, sampler,
        dst, sampler, src, sampler, src,
        dst, sampler, src, sampler, src,
        src, sampler, sampler);

    output_line(ctx, "%s", code);
} // emit_METAL_TEXBEML

EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2AR) // !!! FIXME
EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2GB) // !!! FIXME


static void emit_METAL_TEXM3X2PAD(Context *ctx)
{
    // no-op ... work happens in emit_METAL_TEXM3X2TEX().
} // emit_METAL_TEXM3X2PAD

static void emit_METAL_TEXM3X2TEX(Context *ctx)
{
    if (ctx->texm3x2pad_src0 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char sampler[64];
    char code[512];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_src0,
                            src0, sizeof (src0));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_dst0,
                            src1, sizeof (src1));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src2, sizeof (src2));
    get_METAL_destarg_varname(ctx, dst, sizeof (dst));

    make_METAL_destarg_assign(ctx, code, sizeof (code),
        "%s_texture.sample(%s, float2(dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz)))",
        sampler, sampler, src0, src1, src2, dst);

    output_line(ctx, "%s", code);
} // emit_METAL_TEXM3X2TEX

static void emit_METAL_TEXM3X3PAD(Context *ctx)
{
    // no-op ... work happens in emit_METAL_TEXM3X3*().
} // emit_METAL_TEXM3X3PAD

static void emit_METAL_TEXM3X3TEX(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char sampler[64];
    char code[512];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_METAL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "Cube" : "3D";

    make_METAL_destarg_assign(ctx, code, sizeof (code),
        "texture%s(%s,"
            " float3(dot(%s.xyz, %s.xyz),"
            " dot(%s.xyz, %s.xyz),"
            " dot(%s.xyz, %s.xyz)))",
        ttypestr, sampler, src0, src1, src2, src3, dst, src4);

    output_line(ctx, "%s", code);
} // emit_METAL_TEXM3X3TEX

static void emit_METAL_TEXM3X3SPEC_helper(Context *ctx)
{
    if (ctx->glsl_generated_texm3x3spec_helper)
        return;

    ctx->glsl_generated_texm3x3spec_helper = 1;

    push_output(ctx, &ctx->helpers);
    output_line(ctx, "float3 TEXM3X3SPEC_reflection(const float3 normal, const float3 eyeray)");
    output_line(ctx, "{"); ctx->indent++;
    output_line(ctx,   "return (2.0 * ((normal * eyeray) / (normal * normal)) * normal) - eyeray;"); ctx->indent--;
    output_line(ctx, "}");
    output_blank_line(ctx);
    pop_output(ctx);
} // emit_METAL_TEXM3X3SPEC_helper

static void emit_METAL_TEXM3X3SPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char src5[64];
    char sampler[64];
    char code[512];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    emit_METAL_TEXM3X3SPEC_helper(ctx);

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[1].regnum,
                            src5, sizeof (src5));
    get_METAL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "Cube" : "3D";

    make_METAL_destarg_assign(ctx, code, sizeof (code),
        "texture%s(%s, "
            "TEXM3X3SPEC_reflection("
                "float3("
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz)"
                "),"
                "%s.xyz,"
            ")"
        ")",
        ttypestr, sampler, src0, src1, src2, src3, dst, src4, src5);

    output_line(ctx, "%s", code);
} // emit_METAL_TEXM3X3SPEC

static void emit_METAL_TEXM3X3VSPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    DestArgInfo *info = &ctx->dest_arg;
    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char sampler[64];
    char code[512];

    ctx->metal_need_header_texture = 1;
    ctx->metal_need_header_geometric = 1;

    emit_METAL_TEXM3X3SPEC_helper(ctx);

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_METAL_varname_in_buf(ctx, REG_TYPE_SAMPLER, info->regnum,
                            sampler, sizeof (sampler));

    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_METAL_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      info->regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "Cube" : "3D";

    make_METAL_destarg_assign(ctx, code, sizeof (code),
        "texture%s(%s, "
            "TEXM3X3SPEC_reflection("
                "float3("
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz), "
                    "dot(%s.xyz, %s.xyz)"
                "), "
                "float3(%s.w, %s.w, %s.w)"
            ")"
        ")",
        ttypestr, sampler, src0, src1, src2, src3, dst, src4, src0, src2, dst);

    output_line(ctx, "%s", code);
} // emit_METAL_TEXM3X3VSPEC

static void emit_METAL_EXPP(Context *ctx)
{
    // !!! FIXME: msdn's asm docs don't list this opcode, I'll have to check the driver documentation.
    emit_METAL_EXP(ctx);  // I guess this is just partial precision EXP?
} // emit_METAL_EXPP

static void emit_METAL_LOGP(Context *ctx)
{
    // LOGP is just low-precision LOG, but we'll take the higher precision.
    emit_METAL_LOG(ctx);
} // emit_METAL_LOGP

// common code between CMP and CND.
static void emit_METAL_comparison_operations(Context *ctx, const char *cmp)
{
    int i, j;
    DestArgInfo *dst = &ctx->dest_arg;
    const SourceArgInfo *srcarg0 = &ctx->source_args[0];
    const int origmask = dst->writemask;
    int used_swiz[4] = { 0, 0, 0, 0 };
    const int writemask[4] = { dst->writemask0, dst->writemask1,
                               dst->writemask2, dst->writemask3 };
    const int src0swiz[4] = { srcarg0->swizzle_x, srcarg0->swizzle_y,
                              srcarg0->swizzle_z, srcarg0->swizzle_w };

    for (i = 0; i < 4; i++)
    {
        int mask = (1 << i);

        if (!writemask[i]) continue;
        if (used_swiz[i]) continue;

        // This is a swizzle we haven't checked yet.
        used_swiz[i] = 1;

        // see if there are any other elements swizzled to match (.yyyy)
        for (j = i + 1; j < 4; j++)
        {
            if (!writemask[j]) continue;
            if (src0swiz[i] != src0swiz[j]) continue;
            mask |= (1 << j);
            used_swiz[j] = 1;
        } // for

        // okay, (mask) should be the writemask of swizzles we like.

        //return make_METAL_srcarg_string(ctx, idx, (1 << 0));

        char src0[64];
        char src1[64];
        char src2[64];
        make_METAL_srcarg_string(ctx, 0, (1 << i), src0, sizeof (src0));
        make_METAL_srcarg_string(ctx, 1, mask, src1, sizeof (src1));
        make_METAL_srcarg_string(ctx, 2, mask, src2, sizeof (src2));

        set_dstarg_writemask(dst, mask);

        char code[128];
        make_METAL_destarg_assign(ctx, code, sizeof (code),
                                 "((%s %s) ? %s : %s)",
                                 src0, cmp, src1, src2);
        output_line(ctx, "%s", code);
    } // for

    set_dstarg_writemask(dst, origmask);
} // emit_METAL_comparison_operations

static void emit_METAL_CND(Context *ctx)
{
    emit_METAL_comparison_operations(ctx, "> 0.5");
} // emit_METAL_CND

static void emit_METAL_DEF(Context *ctx)
{
    const float *val = (const float *) ctx->dwords; // !!! FIXME: could be int?
    char varname[64]; get_METAL_destarg_varname(ctx, varname, sizeof (varname));
    char val0[32]; floatstr(ctx, val0, sizeof (val0), val[0], 1);
    char val1[32]; floatstr(ctx, val1, sizeof (val1), val[1], 1);
    char val2[32]; floatstr(ctx, val2, sizeof (val2), val[2], 1);
    char val3[32]; floatstr(ctx, val3, sizeof (val3), val[3], 1);

    push_output(ctx, &ctx->mainline_top);
    ctx->indent++;
    // The "(void) %s;" is to make the compiler not warn if this isn't used.
    output_line(ctx, "const float4 %s = float4(%s, %s, %s, %s); (void) %s;",
                varname, val0, val1, val2, val3, varname);
    pop_output(ctx);
} // emit_METAL_DEF

EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2RGB) // !!! FIXME
EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3TEX) // !!! FIXME
EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X2DEPTH) // !!! FIXME
EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3) // !!! FIXME

static void emit_METAL_TEXM3X3(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char code[512];

    ctx->metal_need_header_geometric = 1;

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_METAL_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_METAL_destarg_varname(ctx, dst, sizeof (dst));

    make_METAL_destarg_assign(ctx, code, sizeof (code),
        "float4(dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz), dot(%s.xyz, %s.xyz), 1.0)",
        src0, src1, src2, src3, dst, src4);

    output_line(ctx, "%s", code);
} // emit_METAL_TEXM3X3

EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(TEXDEPTH) // !!! FIXME

static void emit_METAL_CMP(Context *ctx)
{
    emit_METAL_comparison_operations(ctx, ">= 0.0");
} // emit_METAL_CMP

EMIT_METAL_OPCODE_UNIMPLEMENTED_FUNC(BEM) // !!! FIXME

static void emit_METAL_DP2ADD(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_vec2(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_vec2(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_METAL_srcarg_string_scalar(ctx, 2, src2, sizeof (src2));
    char extra[64]; snprintf(extra, sizeof (extra), " + %s", src2);
    emit_METAL_dotprod(ctx, src0, src1, extra);
} // emit_METAL_DP2ADD

static void emit_METAL_DSX(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_graphics = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "dfdx(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_DSX

static void emit_METAL_DSY(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char code[128];
    ctx->metal_need_header_graphics = 1;
    make_METAL_destarg_assign(ctx, code, sizeof (code), "dfdy(%s)", src0);
    output_line(ctx, "%s", code);
} // emit_METAL_DSY

static void emit_METAL_TEXLDD(Context *ctx)
{
    metal_texld(ctx, 1);
} // emit_METAL_TEXLDD

static void emit_METAL_SETP(Context *ctx)
{
    const int vecsize = vecsize_from_writemask(ctx->dest_arg.writemask);
    char src0[64]; make_METAL_srcarg_string_masked(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_METAL_srcarg_string_masked(ctx, 1, src1, sizeof (src1));
    char code[128];

    // destination is always predicate register (which is type bvec4).
    const char *comp = (vecsize == 1) ?
            get_METAL_comparison_string_scalar(ctx) :
            get_METAL_comparison_string_vector(ctx);

    make_METAL_destarg_assign(ctx, code, sizeof (code),
                              "(%s %s %s)", src0, comp, src1);
    output_line(ctx, "%s", code);
} // emit_METAL_SETP

static void emit_METAL_TEXLDL(Context *ctx)
{
    // !!! FIXME: The spec says we can't use GLSL's texture*Lod() built-ins
    // !!! FIXME:  from fragment shaders for some inexplicable reason.
    // !!! FIXME:  Maybe Metal can do it, but I haven't looked into it yet.
    emit_METAL_TEXLD(ctx);
} // emit_METAL_TEXLDL

static void emit_METAL_BREAKP(Context *ctx)
{
    char src0[64]; make_METAL_srcarg_string_scalar(ctx, 0, src0, sizeof (src0));
    output_line(ctx, "if (%s) { break; }", src0);
} // emit_METAL_BREAKP

static void emit_METAL_RESERVED(Context *ctx)
{
    // do nothing; fails in the state machine.
} // emit_METAL_RESERVED

#endif  // SUPPORT_PROFILE_METAL


#if !SUPPORT_PROFILE_ARB1
#define PROFILE_EMITTER_ARB1(op)
#else
#undef AT_LEAST_ONE_PROFILE
#define AT_LEAST_ONE_PROFILE 1
#define PROFILE_EMITTER_ARB1(op) emit_ARB1_##op,

static inline const char *get_ARB1_register_string(Context *ctx,
                        const RegisterType regtype, const int regnum,
                        char *regnum_str, const size_t regnum_size)
{
    // turns out these are identical at the moment.
    return get_D3D_register_string(ctx,regtype,regnum,regnum_str,regnum_size);
} // get_ARB1_register_string

static const char *allocate_ARB1_scratch_reg_name(Context *ctx, char *buf,
                                                  const size_t buflen)
{
    const int scratch = allocate_scratch_register(ctx);
    snprintf(buf, buflen, "scratch%d", scratch);
    return buf;
} // allocate_ARB1_scratch_reg_name

static inline const char *get_ARB1_branch_label_name(Context *ctx, const int id,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "branch_label%d", id);
    return buf;
} // get_ARB1_branch_label_name

static const char *get_ARB1_varname_in_buf(Context *ctx, const RegisterType rt,
                                           const int regnum, char *buf,
                                           const size_t buflen)
{
    // turns out these are identical at the moment.
    return get_D3D_varname_in_buf(ctx, rt, regnum, buf, buflen);
} // get_ARB1_varname_in_buf

static const char *get_ARB1_varname(Context *ctx, const RegisterType rt,
                                    const int regnum)
{
    // turns out these are identical at the moment.
    return get_D3D_varname(ctx, rt, regnum);
} // get_ARB1_varname


static inline const char *get_ARB1_const_array_varname_in_buf(Context *ctx,
                                                const int base, const int size,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "c_array_%d_%d", base, size);
    return buf;
} // get_ARB1_const_array_varname_in_buf


static const char *get_ARB1_const_array_varname(Context *ctx, int base, int size)
{
    char buf[64];
    get_ARB1_const_array_varname_in_buf(ctx, base, size, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_ARB1_const_array_varname


static const char *make_ARB1_srcarg_string_in_buf(Context *ctx,
                                                  const SourceArgInfo *arg,
                                                  char *buf, size_t buflen)
{
    // !!! FIXME: this can hit pathological cases where we look like this...
    //
    //    dp3 r1.xyz, t0_bx2, t0_bx2
    //    mad r1.xyz, t0_bias, 1-r1, t0_bx2
    //
    // ...which do a lot of duplicate work in arb1...
    //
    //    SUB scratch0, t0, { 0.5, 0.5, 0.5, 0.5 };
    //    MUL scratch0, scratch0, { 2.0, 2.0, 2.0, 2.0 };
    //    SUB scratch1, t0, { 0.5, 0.5, 0.5, 0.5 };
    //    MUL scratch1, scratch1, { 2.0, 2.0, 2.0, 2.0 };
    //    DP3 r1.xyz, scratch0, scratch1;
    //    SUB scratch0, t0, { 0.5, 0.5, 0.5, 0.5 };
    //    SUB scratch1, { 1.0, 1.0, 1.0, 1.0 }, r1;
    //    SUB scratch2, t0, { 0.5, 0.5, 0.5, 0.5 };
    //    MUL scratch2, scratch2, { 2.0, 2.0, 2.0, 2.0 };
    //    MAD r1.xyz, scratch0, scratch1, scratch2;
    //
    // ...notice that the dp3 calculates the same value into two scratch
    //  registers. This case is easier to handle; just see if multiple
    //  source args are identical, build it up once, and use the same
    //  scratch register for multiple arguments in that opcode.
    //  Even better still, only calculate things once across instructions,
    //  and be smart about letting it linger in a scratch register until we
    //  definitely don't need the calculation anymore. That's harder to
    //  write, though.

    char regnum_str[16] = { '\0' };

    // !!! FIXME: use get_ARB1_varname_in_buf() instead?
    const char *regtype_str = NULL;
    if (!arg->relative)
    {
        regtype_str = get_ARB1_register_string(ctx, arg->regtype,
                                               arg->regnum, regnum_str,
                                               sizeof (regnum_str));
    } // if

    const char *rel_lbracket = "";
    char rel_offset[32] = { '\0' };
    const char *rel_rbracket = "";
    char rel_swizzle[4] = { '\0' };
    const char *rel_regtype_str = "";
    if (arg->relative)
    {
        rel_regtype_str = get_ARB1_varname_in_buf(ctx, arg->relative_regtype,
                                                  arg->relative_regnum,
                                                  (char *) alloca(64), 64);

        rel_swizzle[0] = '.';
        rel_swizzle[1] = swizzle_channels[arg->relative_component];
        rel_swizzle[2] = '\0';

        if (!support_nv2(ctx))
        {
            // The address register in ARB1 only allows the '.x' component, so
            //  we need to load the component we need from a temp vector
            //  register into .x as needed.
            assert(arg->relative_regtype == REG_TYPE_ADDRESS);
            assert(arg->relative_regnum == 0);
            if (ctx->last_address_reg_component != arg->relative_component)
            {
                output_line(ctx, "ARL %s.x, addr%d.%c;", rel_regtype_str,
                            arg->relative_regnum,
                            swizzle_channels[arg->relative_component]);
                ctx->last_address_reg_component = arg->relative_component;
            } // if

            rel_swizzle[1] = 'x';
        } // if

        if (arg->regtype == REG_TYPE_INPUT)
            regtype_str = "vertex.attrib";
        else
        {
            assert(arg->regtype == REG_TYPE_CONST);
            const int arrayidx = arg->relative_array->index;
            const int arraysize = arg->relative_array->count;
            const int offset = arg->regnum - arrayidx;
            assert(offset >= 0);
            regtype_str = get_ARB1_const_array_varname_in_buf(ctx, arrayidx,
                                           arraysize, (char *) alloca(64), 64);
            if (offset != 0)
                snprintf(rel_offset, sizeof (rel_offset), " + %d", offset);
        } // else

        rel_lbracket = "[";
        rel_rbracket = "]";
    } // if

    // This is the source register with everything but swizzle and source mods.
    snprintf(buf, buflen, "%s%s%s%s%s%s%s", regtype_str, regnum_str,
             rel_lbracket, rel_regtype_str, rel_swizzle, rel_offset,
             rel_rbracket);

    // Some of the source mods need to generate instructions to a temp
    //  register, in which case we'll replace the register name.
    const SourceMod mod = arg->src_mod;
    const int inplace = ( (mod == SRCMOD_NONE) || (mod == SRCMOD_NEGATE) ||
                          ((mod == SRCMOD_ABS) && support_nv2(ctx)) );

    if (!inplace)
    {
        const size_t len = 64;
        char *stackbuf = (char *) alloca(len);
        regtype_str = allocate_ARB1_scratch_reg_name(ctx, stackbuf, len);
        regnum_str[0] = '\0'; // move value to scratch register.
        rel_lbracket = "";   // scratch register won't use array.
        rel_rbracket = "";
        rel_offset[0] = '\0';
        rel_swizzle[0] = '\0';
        rel_regtype_str = "";
    } // if

    const char *premod_str = "";
    const char *postmod_str = "";
    switch (mod)
    {
        case SRCMOD_NEGATE:
            premod_str = "-";
            break;

        case SRCMOD_BIASNEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_BIAS:
            output_line(ctx, "SUB %s, %s, { 0.5, 0.5, 0.5, 0.5 };",
                        regtype_str, buf);
            break;

        case SRCMOD_SIGNNEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_SIGN:
            output_line(ctx,
                "MAD %s, %s, { 2.0, 2.0, 2.0, 2.0 }, { -1.0, -1.0, -1.0, -1.0 };",
                regtype_str, buf);
            break;

        case SRCMOD_COMPLEMENT:
            output_line(ctx, "SUB %s, { 1.0, 1.0, 1.0, 1.0 }, %s;",
                        regtype_str, buf);
            break;

        case SRCMOD_X2NEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_X2:
            output_line(ctx, "MUL %s, %s, { 2.0, 2.0, 2.0, 2.0 };",
                        regtype_str, buf);
            break;

        case SRCMOD_DZ:
            fail(ctx, "SRCMOD_DZ currently unsupported in arb1");
            postmod_str = "_dz";
            break;

        case SRCMOD_DW:
            fail(ctx, "SRCMOD_DW currently unsupported in arb1");
            postmod_str = "_dw";
            break;

        case SRCMOD_ABSNEGATE:
            premod_str = "-";
            // fall through.
        case SRCMOD_ABS:
            if (!support_nv2(ctx))  // GL_NV_vertex_program2_option adds this.
                output_line(ctx, "ABS %s, %s;", regtype_str, buf);
            else
            {
                premod_str = (mod == SRCMOD_ABSNEGATE) ? "-|" : "|";
                postmod_str = "|";
            } // else
            break;

        case SRCMOD_NOT:
            fail(ctx, "SRCMOD_NOT currently unsupported in arb1");
            premod_str = "!";
            break;

        case SRCMOD_NONE:
        case SRCMOD_TOTAL:
             break;  // stop compiler whining.
    } // switch

    char swizzle_str[6];
    size_t i = 0;

    if (support_nv4(ctx))  // vFace must be output as "vFace.x" in nv4.
    {
        if (arg->regtype == REG_TYPE_MISCTYPE)
        {
            if ( ((const MiscTypeType) arg->regnum) == MISCTYPE_TYPE_FACE )
            {
                swizzle_str[i++] = '.';
                swizzle_str[i++] = 'x';
            } // if
        } // if
    } // if

    const int scalar = isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum);
    if (!scalar && !no_swizzle(arg->swizzle))
    {
        swizzle_str[i++] = '.';

        // .xxxx is the same as .x, but .xx is illegal...scalar or full!
        if (replicate_swizzle(arg->swizzle))
            swizzle_str[i++] = swizzle_channels[arg->swizzle_x];
        else
        {
            swizzle_str[i++] = swizzle_channels[arg->swizzle_x];
            swizzle_str[i++] = swizzle_channels[arg->swizzle_y];
            swizzle_str[i++] = swizzle_channels[arg->swizzle_z];
            swizzle_str[i++] = swizzle_channels[arg->swizzle_w];
        } // else
    } // if
    swizzle_str[i] = '\0';
    assert(i < sizeof (swizzle_str));

    snprintf(buf, buflen, "%s%s%s%s%s%s%s%s%s%s", premod_str,
             regtype_str, regnum_str, rel_lbracket,
             rel_regtype_str, rel_swizzle, rel_offset, rel_rbracket,
             swizzle_str, postmod_str);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_ARB1_srcarg_string_in_buf

static const char *get_ARB1_destarg_varname(Context *ctx, char *buf,
                                            const size_t buflen)
{
    const DestArgInfo *arg = &ctx->dest_arg;
    return get_ARB1_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, buflen);
} // get_ARB1_destarg_varname

static const char *get_ARB1_srcarg_varname(Context *ctx, const size_t idx,
                                           char *buf, const size_t buflen)
{
    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        *buf = '\0';
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];
    return get_ARB1_varname_in_buf(ctx, arg->regtype, arg->regnum, buf, buflen);
} // get_ARB1_srcarg_varname


static const char *make_ARB1_destarg_string(Context *ctx, char *buf,
                                            const size_t buflen)
{
    const DestArgInfo *arg = &ctx->dest_arg;

    *buf = '\0';

    const char *sat_str = "";
    if (arg->result_mod & MOD_SATURATE)
    {
        // nv4 can use ".SAT" in all program types.
        // For less than nv4, the "_SAT" modifier is only available in
        //  fragment shaders. Every thing else will fake it later in
        //  emit_ARB1_dest_modifiers() ...
        if (support_nv4(ctx))
            sat_str = ".SAT";
        else if (shader_is_pixel(ctx))
            sat_str = "_SAT";
    } // if

    const char *pp_str = "";
    if (arg->result_mod & MOD_PP)
    {
        // Most ARB1 profiles can't do partial precision (MOD_PP), but that's
        //  okay. The spec says lots of Direct3D implementations ignore the
        //  flag anyhow.
        if (support_nv4(ctx))
            pp_str = "H";
    } // if

    // CENTROID only allowed in DCL opcodes, which shouldn't come through here.
    assert((arg->result_mod & MOD_CENTROID) == 0);

    char regnum_str[16];
    const char *regtype_str = get_ARB1_register_string(ctx, arg->regtype,
                                                       arg->regnum, regnum_str,
                                                       sizeof (regnum_str));
    if (regtype_str == NULL)
    {
        fail(ctx, "Unknown destination register type.");
        return buf;
    } // if

    char writemask_str[6];
    size_t i = 0;
    const int scalar = isscalar(ctx, ctx->shader_type, arg->regtype, arg->regnum);
    if (!scalar && !writemask_xyzw(arg->writemask))
    {
        writemask_str[i++] = '.';
        if (arg->writemask0) writemask_str[i++] = 'x';
        if (arg->writemask1) writemask_str[i++] = 'y';
        if (arg->writemask2) writemask_str[i++] = 'z';
        if (arg->writemask3) writemask_str[i++] = 'w';
    } // if
    writemask_str[i] = '\0';
    assert(i < sizeof (writemask_str));

    //const char *pred_left = "";
    //const char *pred_right = "";
    char pred[32] = { '\0' };
    if (ctx->predicated)
    {
        fail(ctx, "dest register predication currently unsupported in arb1");
        return buf;
        //pred_left = "(";
        //pred_right = ") ";
        make_ARB1_srcarg_string_in_buf(ctx, &ctx->predicate_arg,
                                       pred, sizeof (pred));
    } // if

    snprintf(buf, buflen, "%s%s %s%s%s", pp_str, sat_str,
             regtype_str, regnum_str, writemask_str);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return buf;
} // make_ARB1_destarg_string


static void emit_ARB1_dest_modifiers(Context *ctx)
{
    const DestArgInfo *arg = &ctx->dest_arg;

    if (arg->result_shift != 0x0)
    {
        char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
        const char *multiplier = NULL;

        switch (arg->result_shift)
        {
            case 0x1: multiplier = "2.0"; break;
            case 0x2: multiplier = "4.0"; break;
            case 0x3: multiplier = "8.0"; break;
            case 0xD: multiplier = "0.125"; break;
            case 0xE: multiplier = "0.25"; break;
            case 0xF: multiplier = "0.5"; break;
        } // switch

        if (multiplier != NULL)
        {
            char var[64]; get_ARB1_destarg_varname(ctx, var, sizeof (var));
            output_line(ctx, "MUL%s, %s, %s;", dst, var, multiplier);
        } // if
    } // if

    if (arg->result_mod & MOD_SATURATE)
    {
        // nv4 and/or pixel shaders just used the "SAT" modifier, instead.
        if ( (!support_nv4(ctx)) && (!shader_is_pixel(ctx)) )
        {
            char var[64]; get_ARB1_destarg_varname(ctx, var, sizeof (var));
            char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
            output_line(ctx, "MIN%s, %s, 1.0;", dst, var);
            output_line(ctx, "MAX%s, %s, 0.0;", dst, var);
        } // if
    } // if
} // emit_ARB1_dest_modifiers


static const char *make_ARB1_srcarg_string(Context *ctx, const size_t idx,
                                           char *buf, const size_t buflen)
{
    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        *buf = '\0';
        return buf;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];
    return make_ARB1_srcarg_string_in_buf(ctx, arg, buf, buflen);
} // make_ARB1_srcarg_string

static void emit_ARB1_opcode_ds(Context *ctx, const char *opcode)
{
    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
    output_line(ctx, "%s%s, %s;", opcode, dst, src0);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_opcode_ds

static void emit_ARB1_opcode_dss(Context *ctx, const char *opcode)
{
    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_ARB1_srcarg_string(ctx, 1, src1, sizeof (src1));
    output_line(ctx, "%s%s, %s, %s;", opcode, dst, src0, src1);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_opcode_dss

static void emit_ARB1_opcode_dsss(Context *ctx, const char *opcode)
{
    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_ARB1_srcarg_string(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_ARB1_srcarg_string(ctx, 2, src2, sizeof (src2));
    output_line(ctx, "%s%s, %s, %s, %s;", opcode, dst, src0, src1, src2);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_opcode_dsss


#define EMIT_ARB1_OPCODE_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_D_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode_d(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_S_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode_s(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_SS_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode_ss(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_DS_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode_ds(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_DSS_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode_dss(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_DSSS_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode_dsss(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_DSSSS_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        emit_ARB1_opcode_dssss(ctx, #op); \
    }
#define EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(op) \
    static void emit_ARB1_##op(Context *ctx) { \
        failf(ctx, #op " unimplemented in %s profile", ctx->profile->name); \
    }


static void emit_ARB1_start(Context *ctx, const char *profilestr)
{
    const char *shader_str = NULL;
    const char *shader_full_str = NULL;
    if (shader_is_vertex(ctx))
    {
        shader_str = "vp";
        shader_full_str = "vertex";
    } // if
    else if (shader_is_pixel(ctx))
    {
        shader_str = "fp";
        shader_full_str = "fragment";
    } // else if
    else
    {
        failf(ctx, "Shader type %u unsupported in this profile.",
              (uint) ctx->shader_type);
        return;
    } // if

    set_output(ctx, &ctx->preflight);

    if (strcmp(profilestr, MOJOSHADER_PROFILE_ARB1) == 0)
        output_line(ctx, "!!ARB%s1.0", shader_str);

    #if SUPPORT_PROFILE_ARB1_NV
    else if (strcmp(profilestr, MOJOSHADER_PROFILE_NV2) == 0)
    {
        ctx->profile_supports_nv2 = 1;
        output_line(ctx, "!!ARB%s1.0", shader_str);
        output_line(ctx, "OPTION NV_%s_program2;", shader_full_str);
    } // else if

    else if (strcmp(profilestr, MOJOSHADER_PROFILE_NV3) == 0)
    {
        // there's no NV_fragment_program3, so just use 2.
        const int ver = shader_is_pixel(ctx) ? 2 : 3;
        ctx->profile_supports_nv2 = 1;
        ctx->profile_supports_nv3 = 1;
        output_line(ctx, "!!ARB%s1.0", shader_str);
        output_line(ctx, "OPTION NV_%s_program%d;", shader_full_str, ver);
    } // else if

    else if (strcmp(profilestr, MOJOSHADER_PROFILE_NV4) == 0)
    {
        ctx->profile_supports_nv2 = 1;
        ctx->profile_supports_nv3 = 1;
        ctx->profile_supports_nv4 = 1;
        output_line(ctx, "!!NV%s4.0", shader_str);
    } // else if
    #endif

    else
    {
        failf(ctx, "Profile '%s' unsupported or unknown.", profilestr);
    } // else

    set_output(ctx, &ctx->mainline);
} // emit_ARB1_start

static void emit_ARB1_end(Context *ctx)
{
    // ps_1_* writes color to r0 instead oC0. We move it to the right place.
    // We don't have to worry about a RET opcode messing this up, since
    //  RET isn't available before ps_2_0.
    if (shader_is_pixel(ctx) && !shader_version_atleast(ctx, 2, 0))
    {
        set_used_register(ctx, REG_TYPE_COLOROUT, 0, 1);
        output_line(ctx, "MOV oC0, r0;");
    } // if

    output_line(ctx, "END");
} // emit_ARB1_end

static void emit_ARB1_phase(Context *ctx)
{
    // no-op in arb1.
} // emit_ARB1_phase

static inline const char *arb1_float_temp(const Context *ctx)
{
    // nv4 lets you specify data type.
    return (support_nv4(ctx)) ? "FLOAT TEMP" : "TEMP";
} // arb1_float_temp

static void emit_ARB1_finalize(Context *ctx)
{
    push_output(ctx, &ctx->preflight);

    if (shader_is_vertex(ctx) && !ctx->arb1_wrote_position)
        output_line(ctx, "OPTION ARB_position_invariant;");

    if (shader_is_pixel(ctx) && ctx->have_multi_color_outputs)
        output_line(ctx, "OPTION ARB_draw_buffers;");

    pop_output(ctx);

    const char *tmpstr = arb1_float_temp(ctx);
    int i;
    push_output(ctx, &ctx->globals);
    for (i = 0; i < ctx->max_scratch_registers; i++)
    {
        char buf[64];
        allocate_ARB1_scratch_reg_name(ctx, buf, sizeof (buf));
        output_line(ctx, "%s %s;", tmpstr, buf);
    } // for

    // nv2 fragment programs (and anything nv4) have a real REP/ENDREP.
    if ( (support_nv2(ctx)) && (!shader_is_pixel(ctx)) && (!support_nv4(ctx)) )
    {
        // set up temps for nv2 REP/ENDREP emulation through branching.
        for (i = 0; i < ctx->max_reps; i++)
            output_line(ctx, "TEMP rep%d;", i);
    } // if

    pop_output(ctx);
    assert(ctx->scratch_registers == ctx->max_scratch_registers);
} // emit_ARB1_finalize

static void emit_ARB1_global(Context *ctx, RegisterType regtype, int regnum)
{
    // !!! FIXME: dependency on ARB1 profile.  // !!! FIXME about FIXME: huh?
    char varname[64];
    get_ARB1_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    push_output(ctx, &ctx->globals);
    switch (regtype)
    {
        case REG_TYPE_ADDRESS:
            if (shader_is_pixel(ctx))  // actually REG_TYPE_TEXTURE.
            {
                // We have to map texture registers to temps for ps_1_1, since
                //  they work like temps, initialize with tex coords, and the
                //  ps_1_1 TEX opcode expects to overwrite it.
                if (!shader_version_atleast(ctx, 1, 4))
                {
                    output_line(ctx, "%s %s;", arb1_float_temp(ctx), varname);
                    push_output(ctx, &ctx->mainline_top);
                    output_line(ctx, "MOV %s, fragment.texcoord[%d];",
                                varname, regnum);
                    pop_output(ctx);
                } // if
                break;
            } // if

            // nv4 replaced address registers with generic int registers.
            if (support_nv4(ctx))
                output_line(ctx, "INT TEMP %s;", varname);
            else
            {
                // nv2 has four-component address already, but stock arb1 has
                //  to emulate it in a temporary, and move components to the
                //  scalar ADDRESS register on demand.
                output_line(ctx, "ADDRESS %s;", varname);
                if (!support_nv2(ctx))
                    output_line(ctx, "TEMP addr%d;", regnum);
            } // else
            break;

        //case REG_TYPE_PREDICATE:
        //    output_line(ctx, "bvec4 %s;", varname);
        //    break;
        case REG_TYPE_TEMP:
            output_line(ctx, "%s %s;", arb1_float_temp(ctx), varname);
            break;
        //case REG_TYPE_LOOP:
        //    break; // no-op. We declare these in for loops at the moment.
        //case REG_TYPE_LABEL:
        //    break; // no-op. If we see it here, it means we optimized it out.
        default:
            fail(ctx, "BUG: we used a register we don't know how to define.");
            break;
    } // switch
    pop_output(ctx);
} // emit_ARB1_global

static void emit_ARB1_array(Context *ctx, VariableList *var)
{
    // All uniforms are now packed tightly into the program.local array,
    //  instead of trying to map them to the d3d registers. So this needs to
    //  map to the next piece of the array we haven't used yet. Thankfully,
    //  arb1 lets you make a PARAM array that maps to a subset of another
    //  array; we don't need to do offsets, since myarray[0] can map to
    //  program.local[5] without any extra math from us.
    const int base = var->index;
    const int size = var->count;
    const int arb1base = ctx->uniform_float4_count +
                         ctx->uniform_int4_count +
                         ctx->uniform_bool_count;
    char varname[64];
    get_ARB1_const_array_varname_in_buf(ctx, base, size, varname, sizeof (varname));
    push_output(ctx, &ctx->globals);
    output_line(ctx, "PARAM %s[%d] = { program.local[%d..%d] };", varname,
                size, arb1base, (arb1base + size) - 1);
    pop_output(ctx);
    var->emit_position = arb1base;
} // emit_ARB1_array

static void emit_ARB1_const_array(Context *ctx, const ConstantsList *clist,
                                  int base, int size)
{
    char varname[64];
    get_ARB1_const_array_varname_in_buf(ctx, base, size, varname, sizeof (varname));
    int i;

    push_output(ctx, &ctx->globals);
    output_line(ctx, "PARAM %s[%d] = {", varname, size);
    ctx->indent++;

    for (i = 0; i < size; i++)
    {
        while (clist->constant.type != MOJOSHADER_UNIFORM_FLOAT)
            clist = clist->next;
        assert(clist->constant.index == (base + i));

        char val0[32];
        char val1[32];
        char val2[32];
        char val3[32];
        floatstr(ctx, val0, sizeof (val0), clist->constant.value.f[0], 1);
        floatstr(ctx, val1, sizeof (val1), clist->constant.value.f[1], 1);
        floatstr(ctx, val2, sizeof (val2), clist->constant.value.f[2], 1);
        floatstr(ctx, val3, sizeof (val3), clist->constant.value.f[3], 1);

        output_line(ctx, "{ %s, %s, %s, %s }%s", val0, val1, val2, val3,
                    (i < (size-1)) ? "," : "");

        clist = clist->next;
    } // for

    ctx->indent--;
    output_line(ctx, "};");
    pop_output(ctx);
} // emit_ARB1_const_array

static void emit_ARB1_uniform(Context *ctx, RegisterType regtype, int regnum,
                              const VariableList *var)
{
    // We pack these down into the program.local array, so if we only use
    //  register c439, it'll actually map to program.local[0]. This will
    //  prevent overflows when we actually have enough resources to run.

    const char *arrayname = "program.local";
    int index = 0;

    char varname[64];
    get_ARB1_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    push_output(ctx, &ctx->globals);

    if (var == NULL)
    {
        // all types share one array (rather, all types convert to float4).
        index = ctx->uniform_float4_count + ctx->uniform_int4_count +
                ctx->uniform_bool_count;
    } // if

    else
    {
        const int arraybase = var->index;
        if (var->constant)
        {
            const int arraysize = var->count;
            arrayname = get_ARB1_const_array_varname_in_buf(ctx, arraybase,
                                        arraysize, (char *) alloca(64), 64);
            index = (regnum - arraybase);
        } // if
        else
        {
            assert(var->emit_position != -1);
            index = (regnum - arraybase) + var->emit_position;
        } // else
    } // else

    output_line(ctx, "PARAM %s = %s[%d];", varname, arrayname, index);
    pop_output(ctx);
} // emit_ARB1_uniform

static void emit_ARB1_sampler(Context *ctx,int stage,TextureType ttype,int tb)
{
    // this is mostly a no-op...you don't predeclare samplers in arb1.

    if (tb)  // This sampler used a ps_1_1 TEXBEM opcode?
    {
        const int index = ctx->uniform_float4_count + ctx->uniform_int4_count +
                          ctx->uniform_bool_count;
        char var[64];
        get_ARB1_varname_in_buf(ctx, REG_TYPE_SAMPLER, stage, var, sizeof(var));
        push_output(ctx, &ctx->globals);
        output_line(ctx, "PARAM %s_texbem = program.local[%d];", var, index);
        output_line(ctx, "PARAM %s_texbeml = program.local[%d];", var, index+1);
        pop_output(ctx);
        ctx->uniform_float4_count += 2;
    } // if
} // emit_ARB1_sampler

// !!! FIXME: a lot of cut-and-paste here from emit_GLSL_attribute().
static void emit_ARB1_attribute(Context *ctx, RegisterType regtype, int regnum,
                                MOJOSHADER_usage usage, int index, int wmask,
                                int flags)
{
    // !!! FIXME: this function doesn't deal with write masks at all yet!
    const char *usage_str = NULL;
    const char *arrayleft = "";
    const char *arrayright = "";
    char index_str[16] = { '\0' };

    char varname[64];
    get_ARB1_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    //assert((flags & MOD_PP) == 0);  // !!! FIXME: is PP allowed?

    if (index != 0)  // !!! FIXME: a lot of these MUST be zero.
        snprintf(index_str, sizeof (index_str), "%u", (uint) index);

    if (shader_is_vertex(ctx))
    {
        // pre-vs3 output registers.
        // these don't ever happen in DCL opcodes, I think. Map to vs_3_*
        //  output registers.
        if (!shader_version_atleast(ctx, 3, 0))
        {
            if (regtype == REG_TYPE_RASTOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                index = regnum;
                switch ((const RastOutType) regnum)
                {
                    case RASTOUT_TYPE_POSITION:
                        usage = MOJOSHADER_USAGE_POSITION;
                        break;
                    case RASTOUT_TYPE_FOG:
                        usage = MOJOSHADER_USAGE_FOG;
                        break;
                    case RASTOUT_TYPE_POINT_SIZE:
                        usage = MOJOSHADER_USAGE_POINTSIZE;
                        break;
                } // switch
            } // if

            else if (regtype == REG_TYPE_ATTROUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_COLOR;
                index = regnum;
            } // else if

            else if (regtype == REG_TYPE_TEXCRDOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_TEXCOORD;
                index = regnum;
            } // else if
        } // if

        // to avoid limitations of various GL entry points for input
        // attributes (glSecondaryColorPointer() can only take 3 component
        // items, glVertexPointer() can't do GL_UNSIGNED_BYTE, many other
        // issues), we set up all inputs as generic vertex attributes, so we
        // can pass data in just about any form, and ignore the built-in GLSL
        // attributes like gl_SecondaryColor. Output needs to use the the
        // built-ins, though, but we don't have to worry about the GL entry
        // point limitations there.

        if (regtype == REG_TYPE_INPUT)
        {
            const int attr = ctx->assigned_vertex_attributes++;
            push_output(ctx, &ctx->globals);
            output_line(ctx, "ATTRIB %s = vertex.attrib[%d];", varname, attr);
            pop_output(ctx);
        } // if

        else if (regtype == REG_TYPE_OUTPUT)
        {
            switch (usage)
            {
                case MOJOSHADER_USAGE_POSITION:
                    ctx->arb1_wrote_position = 1;
                    usage_str = "result.position";
                    break;
                case MOJOSHADER_USAGE_POINTSIZE:
                    usage_str = "result.pointsize";
                    break;
                case MOJOSHADER_USAGE_COLOR:
                    index_str[0] = '\0';  // no explicit number.
                    if (index == 0)
                        usage_str = "result.color.primary";
                    else if (index == 1)
                        usage_str = "result.color.secondary";
                    break;
                case MOJOSHADER_USAGE_FOG:
                    usage_str = "result.fogcoord";
                    break;
                case MOJOSHADER_USAGE_TEXCOORD:
                    snprintf(index_str, sizeof (index_str), "%u", (uint) index);
                    usage_str = "result.texcoord";
                    arrayleft = "[";
                    arrayright = "]";
                    break;
                default:
                    // !!! FIXME: we need to deal with some more built-in varyings here.
                    break;
            } // switch

            // !!! FIXME: the #define is a little hacky, but it means we don't
            // !!! FIXME:  have to track these separately if this works.
            push_output(ctx, &ctx->globals);
            // no mapping to built-in var? Just make it a regular global, pray.
            if (usage_str == NULL)
                output_line(ctx, "%s %s;", arb1_float_temp(ctx), varname);
            else
            {
                output_line(ctx, "OUTPUT %s = %s%s%s%s;", varname, usage_str,
                            arrayleft, index_str, arrayright);
            } // else
            pop_output(ctx);
        } // else if

        else
        {
            fail(ctx, "unknown vertex shader attribute register");
        } // else
    } // if

    else if (shader_is_pixel(ctx))
    {
        const char *paramtype_str = "ATTRIB";

        // samplers DCLs get handled in emit_ARB1_sampler().

        if (flags & MOD_CENTROID)
        {
            if (!support_nv4(ctx))  // GL_NV_fragment_program4 adds centroid.
            {
                // !!! FIXME: should we just wing it without centroid here?
                failf(ctx, "centroid unsupported in %s profile",
                      ctx->profile->name);
                return;
            } // if

            paramtype_str = "CENTROID ATTRIB";
        } // if

        if (regtype == REG_TYPE_COLOROUT)
        {
            paramtype_str = "OUTPUT";
            usage_str = "result.color";
            if (ctx->have_multi_color_outputs)
            {
                // We have to gamble that you have GL_ARB_draw_buffers.
                // You probably do at this point if you have a sane setup.
                snprintf(index_str, sizeof (index_str), "%u", (uint) regnum);
                arrayleft = "[";
                arrayright = "]";
            } // if
        } // if

        else if (regtype == REG_TYPE_DEPTHOUT)
        {
            paramtype_str = "OUTPUT";
            usage_str = "result.depth";
        } // else if

        // !!! FIXME: can you actualy have a texture register with COLOR usage?
        else if ((regtype == REG_TYPE_TEXTURE) || (regtype == REG_TYPE_INPUT))
        {
            if (usage == MOJOSHADER_USAGE_TEXCOORD)
            {
                // ps_1_1 does a different hack for this attribute.
                //  Refer to emit_ARB1_global()'s REG_TYPE_TEXTURE code.
                if (shader_version_atleast(ctx, 1, 4))
                {
                    snprintf(index_str, sizeof (index_str), "%u", (uint) index);
                    usage_str = "fragment.texcoord";
                    arrayleft = "[";
                    arrayright = "]";
                } // if
            } // if

            else if (usage == MOJOSHADER_USAGE_COLOR)
            {
                index_str[0] = '\0';  // no explicit number.
                if (index == 0)
                    usage_str = "fragment.color.primary";
                else if (index == 1)
                    usage_str = "fragment.color.secondary";
                else
                    fail(ctx, "unsupported color index");
            } // else if
        } // else if

        else if (regtype == REG_TYPE_MISCTYPE)
        {
            const MiscTypeType mt = (MiscTypeType) regnum;
            if (mt == MISCTYPE_TYPE_FACE)
            {
                if (support_nv4(ctx))  // FINALLY, a vFace equivalent in nv4!
                {
                    index_str[0] = '\0';  // no explicit number.
                    usage_str = "fragment.facing";
                } // if
                else
                {
                    failf(ctx, "vFace unsupported in %s profile",
                          ctx->profile->name);
                } // else
            } // if
            else if (mt == MISCTYPE_TYPE_POSITION)
            {
                index_str[0] = '\0';  // no explicit number.
                usage_str = "fragment.position";  // !!! FIXME: is this the same coord space as D3D?
            } // else if
            else
            {
                fail(ctx, "BUG: unhandled misc register");
            } // else
        } // else if

        else
        {
            fail(ctx, "unknown pixel shader attribute register");
        } // else

        if (usage_str != NULL)
        {
            push_output(ctx, &ctx->globals);
            output_line(ctx, "%s %s = %s%s%s%s;", paramtype_str, varname,
                        usage_str, arrayleft, index_str, arrayright);
            pop_output(ctx);
        } // if
    } // else if

    else
    {
        fail(ctx, "Unknown shader type");  // state machine should catch this.
    } // else
} // emit_ARB1_attribute

static void emit_ARB1_RESERVED(Context *ctx) { /* no-op. */ }

static void emit_ARB1_NOP(Context *ctx)
{
    // There is no NOP in arb1. Just don't output anything here.
} // emit_ARB1_NOP

EMIT_ARB1_OPCODE_DS_FUNC(MOV)
EMIT_ARB1_OPCODE_DSS_FUNC(ADD)
EMIT_ARB1_OPCODE_DSS_FUNC(SUB)
EMIT_ARB1_OPCODE_DSSS_FUNC(MAD)
EMIT_ARB1_OPCODE_DSS_FUNC(MUL)
EMIT_ARB1_OPCODE_DS_FUNC(RCP)

static void emit_ARB1_RSQ(Context *ctx)
{
    // nv4 doesn't force abs() on this, so negative values will generate NaN.
    // The spec says you should force the abs() yourself.
    if (!support_nv4(ctx))
    {
        emit_ARB1_opcode_ds(ctx, "RSQ");  // pre-nv4 implies ABS.
        return;
    } // if

    // we can optimize this to use nv2's |abs| construct in some cases.
    if ( (ctx->source_args[0].src_mod == SRCMOD_NONE) ||
         (ctx->source_args[0].src_mod == SRCMOD_NEGATE) ||
         (ctx->source_args[0].src_mod == SRCMOD_ABSNEGATE) )
        ctx->source_args[0].src_mod = SRCMOD_ABS;

    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));

    if (ctx->source_args[0].src_mod == SRCMOD_ABS)
        output_line(ctx, "RSQ%s, %s;", dst, src0);
    else
    {
        char buf[64]; allocate_ARB1_scratch_reg_name(ctx, buf, sizeof (buf));
        output_line(ctx, "ABS %s, %s;", buf, src0);
        output_line(ctx, "RSQ%s, %s.x;", dst, buf);
    } // else

    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_RSQ

EMIT_ARB1_OPCODE_DSS_FUNC(DP3)
EMIT_ARB1_OPCODE_DSS_FUNC(DP4)
EMIT_ARB1_OPCODE_DSS_FUNC(MIN)
EMIT_ARB1_OPCODE_DSS_FUNC(MAX)
EMIT_ARB1_OPCODE_DSS_FUNC(SLT)
EMIT_ARB1_OPCODE_DSS_FUNC(SGE)

static void emit_ARB1_EXP(Context *ctx) { emit_ARB1_opcode_ds(ctx, "EX2"); }

static void arb1_log(Context *ctx, const char *opcode)
{
    // !!! FIXME: SRCMOD_NEGATE can be made into SRCMOD_ABS here, too
    // we can optimize this to use nv2's |abs| construct in some cases.
    if ( (ctx->source_args[0].src_mod == SRCMOD_NONE) ||
         (ctx->source_args[0].src_mod == SRCMOD_ABSNEGATE) )
        ctx->source_args[0].src_mod = SRCMOD_ABS;

    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));

    if (ctx->source_args[0].src_mod == SRCMOD_ABS)
        output_line(ctx, "%s%s, %s;", opcode, dst, src0);
    else
    {
        char buf[64]; allocate_ARB1_scratch_reg_name(ctx, buf, sizeof (buf));
        output_line(ctx, "ABS %s, %s;", buf, src0);
        output_line(ctx, "%s%s, %s.x;", opcode, dst, buf);
    } // else

    emit_ARB1_dest_modifiers(ctx);
} // arb1_log


static void emit_ARB1_LOG(Context *ctx)
{
    arb1_log(ctx, "LG2");
} // emit_ARB1_LOG


EMIT_ARB1_OPCODE_DS_FUNC(LIT)
EMIT_ARB1_OPCODE_DSS_FUNC(DST)

static void emit_ARB1_LRP(Context *ctx)
{
    if (shader_is_pixel(ctx))  // fragment shaders have a matching LRP opcode.
        emit_ARB1_opcode_dsss(ctx, "LRP");
    else
    {
        char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
        char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
        char src1[64]; make_ARB1_srcarg_string(ctx, 1, src1, sizeof (src1));
        char src2[64]; make_ARB1_srcarg_string(ctx, 2, src2, sizeof (src2));
        char buf[64]; allocate_ARB1_scratch_reg_name(ctx, buf, sizeof (buf));

        // LRP is: dest = src2 + src0 * (src1 - src2)
        output_line(ctx, "SUB %s, %s, %s;", buf, src1, src2);
        output_line(ctx, "MAD%s, %s, %s, %s;", dst, buf, src0, src2);
        emit_ARB1_dest_modifiers(ctx);
    } // else
} // emit_ARB1_LRP

EMIT_ARB1_OPCODE_DS_FUNC(FRC)

static void arb1_MxXy(Context *ctx, const int x, const int y)
{
    DestArgInfo *dstarg = &ctx->dest_arg;
    const int origmask = dstarg->writemask;
    char src0[64];
    int i;

    make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));

    for (i = 0; i < y; i++)
    {
        char dst[64];
        char row[64];
        make_ARB1_srcarg_string(ctx, i + 1, row, sizeof (row));
        set_dstarg_writemask(dstarg, 1 << i);
        make_ARB1_destarg_string(ctx, dst, sizeof (dst));
        output_line(ctx, "DP%d%s, %s, %s;", x, dst, src0, row);
    } // for

    set_dstarg_writemask(dstarg, origmask);
    emit_ARB1_dest_modifiers(ctx);
} // arb1_MxXy

static void emit_ARB1_M4X4(Context *ctx) { arb1_MxXy(ctx, 4, 4); }
static void emit_ARB1_M4X3(Context *ctx) { arb1_MxXy(ctx, 4, 3); }
static void emit_ARB1_M3X4(Context *ctx) { arb1_MxXy(ctx, 3, 4); }
static void emit_ARB1_M3X3(Context *ctx) { arb1_MxXy(ctx, 3, 3); }
static void emit_ARB1_M3X2(Context *ctx) { arb1_MxXy(ctx, 3, 2); }

static void emit_ARB1_CALL(Context *ctx)
{
    if (!support_nv2(ctx))  // no branching in stock ARB1.
    {
        failf(ctx, "branching unsupported in %s profile", ctx->profile->name);
        return;
    } // if

    char labelstr[64];
    get_ARB1_srcarg_varname(ctx, 0, labelstr, sizeof (labelstr));
    output_line(ctx, "CAL %s;", labelstr);
} // emit_ARB1_CALL

static void emit_ARB1_CALLNZ(Context *ctx)
{
    // !!! FIXME: if src1 is a constbool that's true, we can remove the
    // !!! FIXME:  if. If it's false, we can make this a no-op.

    if (!support_nv2(ctx))  // no branching in stock ARB1.
        failf(ctx, "branching unsupported in %s profile", ctx->profile->name);
    else
    {
        // !!! FIXME: double-check this.
        char labelstr[64];
        char scratch[64];
        char src1[64];
        get_ARB1_srcarg_varname(ctx, 0, labelstr, sizeof (labelstr));
        get_ARB1_srcarg_varname(ctx, 1, src1, sizeof (src1));
        allocate_ARB1_scratch_reg_name(ctx, scratch, sizeof (scratch));
        output_line(ctx, "MOVC %s, %s;", scratch, src1);
        output_line(ctx, "CAL %s (NE.x);", labelstr);
    } // else
} // emit_ARB1_CALLNZ

// !!! FIXME: needs BRA in nv2, LOOP in nv2 fragment progs, and REP in nv4.
EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(LOOP)

static void emit_ARB1_RET(Context *ctx)
{
    // don't fail() if no nv2...maybe we're just ending the mainline?
    //  if we're ending a LABEL that had no CALL, this would all be written
    //  to ctx->ignore anyhow, so this should be "safe" ... arb1 profile will
    //  just end up throwing all this code out.
    if (support_nv2(ctx))  // no branching in stock ARB1.
        output_line(ctx, "RET;");
    set_output(ctx, &ctx->mainline); // in case we were ignoring this function.
} // emit_ARB1_RET


EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(ENDLOOP)

static void emit_ARB1_LABEL(Context *ctx)
{
    if (!support_nv2(ctx))  // no branching in stock ARB1.
        return;  // don't fail()...maybe we never use it, but do fail in CALL.

    const int label = ctx->source_args[0].regnum;
    RegisterList *reg = reglist_find(&ctx->used_registers, REG_TYPE_LABEL, label);

    // MSDN specs say CALL* has to come before the LABEL, so we know if we
    //  can ditch the entire function here as unused.
    if (reg == NULL)
        set_output(ctx, &ctx->ignore);  // Func not used. Parse, but don't output.

    // !!! FIXME: it would be nice if we could determine if a function is
    // !!! FIXME:  only called once and, if so, forcibly inline it.

    //const char *uses_loopreg = ((reg) && (reg->misc == 1)) ? "int aL" : "";
    char labelstr[64];
    get_ARB1_srcarg_varname(ctx, 0, labelstr, sizeof (labelstr));
    output_line(ctx, "%s:", labelstr);
} // emit_ARB1_LABEL


static void emit_ARB1_POW(Context *ctx)
{
    // we can optimize this to use nv2's |abs| construct in some cases.
    if ( (ctx->source_args[0].src_mod == SRCMOD_NONE) ||
         (ctx->source_args[0].src_mod == SRCMOD_ABSNEGATE) )
        ctx->source_args[0].src_mod = SRCMOD_ABS;

    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_ARB1_srcarg_string(ctx, 1, src1, sizeof (src1));

    if (ctx->source_args[0].src_mod == SRCMOD_ABS)
        output_line(ctx, "POW%s, %s, %s;", dst, src0, src1);
    else
    {
        char buf[64]; allocate_ARB1_scratch_reg_name(ctx, buf, sizeof (buf));
        output_line(ctx, "ABS %s, %s;", buf, src0);
        output_line(ctx, "POW%s, %s.x, %s;", dst, buf, src1);
    } // else

    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_POW

static void emit_ARB1_CRS(Context *ctx) { emit_ARB1_opcode_dss(ctx, "XPD"); }

static void emit_ARB1_SGN(Context *ctx)
{
    if (support_nv2(ctx))
        emit_ARB1_opcode_ds(ctx, "SSG");
    else
    {
        char dst[64];
        char src0[64];
        char scratch1[64];
        char scratch2[64];
        make_ARB1_destarg_string(ctx, dst, sizeof (dst));
        make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
        allocate_ARB1_scratch_reg_name(ctx, scratch1, sizeof (scratch1));
        allocate_ARB1_scratch_reg_name(ctx, scratch2, sizeof (scratch2));
        output_line(ctx, "SLT %s, %s, 0.0;", scratch1, src0);
        output_line(ctx, "SLT %s, -%s, 0.0;", scratch2, src0);
        output_line(ctx, "ADD%s -%s, %s;", dst, scratch1, scratch2);
        emit_ARB1_dest_modifiers(ctx);
    } // else
} // emit_ARB1_SGN

EMIT_ARB1_OPCODE_DS_FUNC(ABS)

static void emit_ARB1_NRM(Context *ctx)
{
    // nv2 fragment programs (and anything nv4) have a real NRM.
    if ( (support_nv4(ctx)) || ((support_nv2(ctx)) && (shader_is_pixel(ctx))) )
        emit_ARB1_opcode_ds(ctx, "NRM");
    else
    {
        char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
        char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
        char buf[64]; allocate_ARB1_scratch_reg_name(ctx, buf, sizeof (buf));
        output_line(ctx, "DP3 %s.w, %s, %s;", buf, src0, src0);
        output_line(ctx, "RSQ %s.w, %s.w;", buf, buf);
        output_line(ctx, "MUL%s, %s.w, %s;", dst, buf, src0);
        emit_ARB1_dest_modifiers(ctx);
    } // else
} // emit_ARB1_NRM


static void emit_ARB1_SINCOS(Context *ctx)
{
    // we don't care about the temp registers that <= sm2 demands; ignore them.
    const int mask = ctx->dest_arg.writemask;

    // arb1 fragment programs and everything nv4 have sin/cos/sincos opcodes.
    if ((shader_is_pixel(ctx)) || (support_nv4(ctx)))
    {
        char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
        char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
        if (writemask_x(mask))
            output_line(ctx, "COS%s, %s;", dst, src0);
        else if (writemask_y(mask))
            output_line(ctx, "SIN%s, %s;", dst, src0);
        else if (writemask_xy(mask))
            output_line(ctx, "SCS%s, %s;", dst, src0);
    } // if

    // nv2+ profiles have sin and cos opcodes.
    else if (support_nv2(ctx))
    {
        char dst[64]; get_ARB1_destarg_varname(ctx, dst, sizeof (dst));
        char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
        if (writemask_x(mask))
            output_line(ctx, "COS %s.x, %s;", dst, src0);
        else if (writemask_y(mask))
            output_line(ctx, "SIN %s.y, %s;", dst, src0);
        else if (writemask_xy(mask))
        {
            output_line(ctx, "SIN %s.x, %s;", dst, src0);
            output_line(ctx, "COS %s.y, %s;", dst, src0);
        } // else if
    } // if

    else  // big nasty.
    {
        char dst[64]; get_ARB1_destarg_varname(ctx, dst, sizeof (dst));
        char src0[64]; get_ARB1_srcarg_varname(ctx, 0, src0, sizeof (src0));
        const int need_sin = (writemask_x(mask) || writemask_xy(mask));
        const int need_cos = (writemask_y(mask) || writemask_xy(mask));
        char scratch[64];

        if (need_sin || need_cos)
            allocate_ARB1_scratch_reg_name(ctx, scratch, sizeof (scratch));

        // These sin() and cos() approximations originally found here:
        //    http://www.devmaster.net/forums/showthread.php?t=5784
        //
        // const float B = 4.0f / M_PI;
        // const float C = -4.0f / (M_PI * M_PI);
        // float y = B * x + C * x * fabs(x);
        //
        // // optional better precision...
        // const float P = 0.225f;
        // y = P * (y * fabs(y) - y) + y;
        //
        //
        // That first thing can be reduced to:
        // const float y = ((1.2732395447351626861510701069801f * x) +
        //             ((-0.40528473456935108577551785283891f * x) * fabs(x)));

        if (need_sin)
        {
            // !!! FIXME: use SRCMOD_ABS here?
            output_line(ctx, "ABS %s.x, %s.x;", dst, src0);
            output_line(ctx, "MUL %s.x, %s.x, -0.40528473456935108577551785283891;", dst, dst);
            output_line(ctx, "MUL %s.x, %s.x, 1.2732395447351626861510701069801;", scratch, src0);
            output_line(ctx, "MAD %s.x, %s.x, %s.x, %s.x;", dst, dst, src0, scratch);
        } // if

        // cosine is sin(x + M_PI/2), but you have to wrap x to pi:
        //  if (x+(M_PI/2) > M_PI)
        //      x -= 2 * M_PI;
        //
        // which is...
        //  if (x+(1.57079637050628662109375) > 3.1415927410125732421875)
        //      x += -6.283185482025146484375;

        if (need_cos)
        {
            output_line(ctx, "ADD %s.x, %s.x, 1.57079637050628662109375;", scratch, src0);
            output_line(ctx, "SGE %s.y, %s.x, 3.1415927410125732421875;", scratch, scratch);
            output_line(ctx, "MAD %s.x, %s.y, -6.283185482025146484375, %s.x;", scratch, scratch, scratch);
            output_line(ctx, "ABS %s.x, %s.x;", dst, src0);
            output_line(ctx, "MUL %s.x, %s.x, -0.40528473456935108577551785283891;", dst, dst);
            output_line(ctx, "MUL %s.x, %s.x, 1.2732395447351626861510701069801;", scratch, src0);
            output_line(ctx, "MAD %s.y, %s.x, %s.x, %s.x;", dst, dst, src0, scratch);
        } // if
    } // else

    // !!! FIXME: might not have done anything. Don't emit if we didn't.
    if (!isfail(ctx))
        emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_SINCOS


static void emit_ARB1_REP(Context *ctx)
{
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));

    // nv2 fragment programs (and everything nv4) have a real REP.
    if ( (support_nv4(ctx)) || ((support_nv2(ctx)) && (shader_is_pixel(ctx))) )
        output_line(ctx, "REP %s;", src0);

    else if (support_nv2(ctx))
    {
        // no REP, but we can use branches.
        char failbranch[32];
        char topbranch[32];
        const int toplabel = allocate_branch_label(ctx);
        const int faillabel = allocate_branch_label(ctx);
        get_ARB1_branch_label_name(ctx,faillabel,failbranch,sizeof(failbranch));
        get_ARB1_branch_label_name(ctx,toplabel,topbranch,sizeof(topbranch));

        assert(((size_t) ctx->branch_labels_stack_index) <
                STATICARRAYLEN(ctx->branch_labels_stack)-1);

        ctx->branch_labels_stack[ctx->branch_labels_stack_index++] = toplabel;
        ctx->branch_labels_stack[ctx->branch_labels_stack_index++] = faillabel;

        char scratch[32];
        snprintf(scratch, sizeof (scratch), "rep%d", ctx->reps);
        output_line(ctx, "MOVC %s.x, %s;", scratch, src0);
        output_line(ctx, "BRA %s (LE.x);", failbranch);
        output_line(ctx, "%s:", topbranch);
    } // else if

    else  // stock ARB1 has no branching.
    {
        fail(ctx, "branching unsupported in this profile");
    } // else
} // emit_ARB1_REP


static void emit_ARB1_ENDREP(Context *ctx)
{
    // nv2 fragment programs (and everything nv4) have a real ENDREP.
    if ( (support_nv4(ctx)) || ((support_nv2(ctx)) && (shader_is_pixel(ctx))) )
        output_line(ctx, "ENDREP;");

    else if (support_nv2(ctx))
    {
        // no ENDREP, but we can use branches.
        assert(ctx->branch_labels_stack_index >= 2);

        char failbranch[32];
        char topbranch[32];
        const int faillabel = ctx->branch_labels_stack[--ctx->branch_labels_stack_index];
        const int toplabel = ctx->branch_labels_stack[--ctx->branch_labels_stack_index];
        get_ARB1_branch_label_name(ctx,faillabel,failbranch,sizeof(failbranch));
        get_ARB1_branch_label_name(ctx,toplabel,topbranch,sizeof(topbranch));

        char scratch[32];
        snprintf(scratch, sizeof (scratch), "rep%d", ctx->reps);
        output_line(ctx, "SUBC %s.x, %s.x, 1.0;", scratch, scratch);
        output_line(ctx, "BRA %s (GT.x);", topbranch);
        output_line(ctx, "%s:", failbranch);
    } // else if

    else  // stock ARB1 has no branching.
    {
        fail(ctx, "branching unsupported in this profile");
    } // else
} // emit_ARB1_ENDREP


static void nv2_if(Context *ctx)
{
    // The condition code register MUST be set up before this!
    // nv2 fragment programs (and everything nv4) have a real IF.
    if ( (support_nv4(ctx)) || (shader_is_pixel(ctx)) )
        output_line(ctx, "IF EQ.x;");
    else
    {
        // there's no IF construct, but we can use a branch to a label.
        char failbranch[32];
        const int label = allocate_branch_label(ctx);
        get_ARB1_branch_label_name(ctx, label, failbranch, sizeof (failbranch));

        assert(((size_t) ctx->branch_labels_stack_index)
                 < STATICARRAYLEN(ctx->branch_labels_stack));

        ctx->branch_labels_stack[ctx->branch_labels_stack_index++] = label;

        // !!! FIXME: should this be NE? (EQ would jump to the ELSE for the IF condition, right?).
        output_line(ctx, "BRA %s (EQ.x);", failbranch);
    } // else
} // nv2_if


static void emit_ARB1_IF(Context *ctx)
{
    if (support_nv2(ctx))
    {
        char buf[64]; allocate_ARB1_scratch_reg_name(ctx, buf, sizeof (buf));
        char src0[64]; get_ARB1_srcarg_varname(ctx, 0, src0, sizeof (src0));
        output_line(ctx, "MOVC %s.x, %s;", buf, src0);
        nv2_if(ctx);
    } // if

    else  // stock ARB1 has no branching.
    {
        failf(ctx, "branching unsupported in %s profile", ctx->profile->name);
    } // else
} // emit_ARB1_IF


static void emit_ARB1_ELSE(Context *ctx)
{
    // nv2 fragment programs (and everything nv4) have a real ELSE.
    if ( (support_nv4(ctx)) || ((support_nv2(ctx)) && (shader_is_pixel(ctx))) )
        output_line(ctx, "ELSE;");

    else if (support_nv2(ctx))
    {
        // there's no ELSE construct, but we can use a branch to a label.
        assert(ctx->branch_labels_stack_index > 0);

        // At the end of the IF block, unconditionally jump to the ENDIF.
        const int endlabel = allocate_branch_label(ctx);
        char endbranch[32];
        get_ARB1_branch_label_name(ctx,endlabel,endbranch,sizeof (endbranch));
        output_line(ctx, "BRA %s;", endbranch);

        // Now mark the ELSE section with a lable.
        const int elselabel = ctx->branch_labels_stack[ctx->branch_labels_stack_index-1];
        char elsebranch[32];
        get_ARB1_branch_label_name(ctx,elselabel,elsebranch,sizeof(elsebranch));
        output_line(ctx, "%s:", elsebranch);

        // Replace the ELSE label with the ENDIF on the label stack.
        ctx->branch_labels_stack[ctx->branch_labels_stack_index-1] = endlabel;
    } // else if

    else  // stock ARB1 has no branching.
    {
        failf(ctx, "branching unsupported in %s profile", ctx->profile->name);
    } // else
} // emit_ARB1_ELSE


static void emit_ARB1_ENDIF(Context *ctx)
{
    // nv2 fragment programs (and everything nv4) have a real ENDIF.
    if ( (support_nv4(ctx)) || ((support_nv2(ctx)) && (shader_is_pixel(ctx))) )
        output_line(ctx, "ENDIF;");

    else if (support_nv2(ctx))
    {
        // there's no ENDIF construct, but we can use a branch to a label.
        assert(ctx->branch_labels_stack_index > 0);
        const int endlabel = ctx->branch_labels_stack[--ctx->branch_labels_stack_index];
        char endbranch[32];
        get_ARB1_branch_label_name(ctx,endlabel,endbranch,sizeof (endbranch));
        output_line(ctx, "%s:", endbranch);
    } // if

    else  // stock ARB1 has no branching.
    {
        failf(ctx, "branching unsupported in %s profile", ctx->profile->name);
    } // else
} // emit_ARB1_ENDIF


static void emit_ARB1_BREAK(Context *ctx)
{
    // nv2 fragment programs (and everything nv4) have a real BREAK.
    if ( (support_nv4(ctx)) || ((support_nv2(ctx)) && (shader_is_pixel(ctx))) )
        output_line(ctx, "BRK;");

    else if (support_nv2(ctx))
    {
        // no BREAK, but we can use branches.
        assert(ctx->branch_labels_stack_index >= 2);
        const int faillabel = ctx->branch_labels_stack[ctx->branch_labels_stack_index];
        char failbranch[32];
        get_ARB1_branch_label_name(ctx,faillabel,failbranch,sizeof(failbranch));
        output_line(ctx, "BRA %s;", failbranch);
    } // else if

    else  // stock ARB1 has no branching.
    {
        failf(ctx, "branching unsupported in %s profile", ctx->profile->name);
    } // else
} // emit_ARB1_BREAK


static void emit_ARB1_MOVA(Context *ctx)
{
    // nv2 and nv3 can use the ARR opcode.
    // But nv4 removed ARR (and ADDRESS registers!). Just ROUND to an INT.
    if (support_nv4(ctx))
        emit_ARB1_opcode_ds(ctx, "ROUND.S");  // !!! FIXME: don't use a modifier here.
    else if ((support_nv2(ctx)) || (support_nv3(ctx)))
        emit_ARB1_opcode_ds(ctx, "ARR");
    else
    {
        char src0[64];
        char scratch[64];
        char addr[32];

        make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
        allocate_ARB1_scratch_reg_name(ctx, scratch, sizeof (scratch));
        snprintf(addr, sizeof (addr), "addr%d", ctx->dest_arg.regnum);

        // !!! FIXME: we can optimize this if src_mod is ABS or ABSNEGATE.

        // ARL uses floor(), but D3D expects round-to-nearest.
        // There is probably a more efficient way to do this.
        if (shader_is_pixel(ctx))  // CMP only exists in fragment programs.  :/
            output_line(ctx, "CMP %s, %s, -1.0, 1.0;", scratch, src0);
        else
        {
            output_line(ctx, "SLT %s, %s, 0.0;", scratch, src0);
            output_line(ctx, "MAD %s, %s, -2.0, 1.0;", scratch, scratch);
        } // else

        output_line(ctx, "ABS %s, %s;", addr, src0);
        output_line(ctx, "ADD %s, %s, 0.5;", addr, addr);
        output_line(ctx, "FLR %s, %s;", addr, addr);
        output_line(ctx, "MUL %s, %s, %s;", addr, addr, scratch);

        // we don't handle these right now, since emit_ARB1_dest_modifiers(ctx)
        //  wants to look at dest_arg, not our temp register.
        assert(ctx->dest_arg.result_mod == 0);
        assert(ctx->dest_arg.result_shift == 0);

        // we assign to the actual address register as needed.
        ctx->last_address_reg_component = -1;
    } // else
} // emit_ARB1_MOVA


static void emit_ARB1_TEXKILL(Context *ctx)
{
    // d3d kills on xyz, arb1 kills on xyzw. Fix the swizzle.
    //  We just map the x component to w. If it's negative, the fragment
    //  would discard anyhow, otherwise, it'll pass through okay. This saves
    //  us a temp register.
    char dst[64];
    get_ARB1_destarg_varname(ctx, dst, sizeof (dst));
    output_line(ctx, "KIL %s.xyzx;", dst);
} // emit_ARB1_TEXKILL

static void arb1_texbem(Context *ctx, const int luminance)
{
    // !!! FIXME: this code counts on the register not having swizzles, etc.
    const int stage = ctx->dest_arg.regnum;
    char dst[64]; get_ARB1_destarg_varname(ctx, dst, sizeof (dst));
    char src[64]; get_ARB1_srcarg_varname(ctx, 0, src, sizeof (src));
    char tmp[64]; allocate_ARB1_scratch_reg_name(ctx, tmp, sizeof (tmp));
    char sampler[64];
    get_ARB1_varname_in_buf(ctx, REG_TYPE_SAMPLER, stage,
                            sampler, sizeof (sampler));

    output_line(ctx, "MUL %s, %s_texbem.xzyw, %s.xyxy;", tmp, sampler, src);
    output_line(ctx, "ADD %s.xy, %s.xzxx, %s.ywxx;", tmp, tmp, tmp);
    output_line(ctx, "ADD %s.xy, %s, %s;", tmp, tmp, dst);
    output_line(ctx, "TEX %s, %s, texture[%d], 2D;", dst, tmp, stage);

    if (luminance)  // TEXBEML, not just TEXBEM?
    {
        output_line(ctx, "MAD %s, %s.zzzz, %s_texbeml.xxxx, %s_texbeml.yyyy;",
                    tmp, src, sampler, sampler);
        output_line(ctx, "MUL %s, %s, %s;", dst, dst, tmp);
    } // if

    emit_ARB1_dest_modifiers(ctx);
} // arb1_texbem

static void emit_ARB1_TEXBEM(Context *ctx)
{
    arb1_texbem(ctx, 0);
} // emit_ARB1_TEXBEM

static void emit_ARB1_TEXBEML(Context *ctx)
{
    arb1_texbem(ctx, 1);
} // emit_ARB1_TEXBEML

EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2AR)
EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2GB)


static void emit_ARB1_TEXM3X2PAD(Context *ctx)
{
    // no-op ... work happens in emit_ARB1_TEXM3X2TEX().
} // emit_ARB1_TEXM3X2PAD

static void emit_ARB1_TEXM3X2TEX(Context *ctx)
{
    if (ctx->texm3x2pad_src0 == -1)
        return;

    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    const int stage = ctx->dest_arg.regnum;
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_src0,
                            src0, sizeof (src0));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x2pad_dst0,
                            src1, sizeof (src1));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src2, sizeof (src2));
    get_ARB1_destarg_varname(ctx, dst, sizeof (dst));

    output_line(ctx, "DP3 %s.y, %s, %s;", dst, src2, dst);
    output_line(ctx, "DP3 %s.x, %s, %s;", dst, src0, src1);
    output_line(ctx, "TEX %s, %s, texture[%d], 2D;", dst, dst, stage);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_TEXM3X2TEX


static void emit_ARB1_TEXM3X3PAD(Context *ctx)
{
    // no-op ... work happens in emit_ARB1_TEXM3X3*().
} // emit_ARB1_TEXM3X3PAD


static void emit_ARB1_TEXM3X3TEX(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    const int stage = ctx->dest_arg.regnum;
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_ARB1_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER, stage);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "CUBE" : "3D";

    output_line(ctx, "DP3 %s.z, %s, %s;", dst, dst, src4);
    output_line(ctx, "DP3 %s.x, %s, %s;", dst, src0, src1);
    output_line(ctx, "DP3 %s.y, %s, %s;", dst, src2, src3);
    output_line(ctx, "TEX %s, %s, texture[%d], %s;", dst, dst, stage, ttypestr);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_TEXM3X3TEX

static void emit_ARB1_TEXM3X3SPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char src5[64];
    char tmp[64];
    char tmp2[64];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    const int stage = ctx->dest_arg.regnum;
    allocate_ARB1_scratch_reg_name(ctx, tmp, sizeof (tmp));
    allocate_ARB1_scratch_reg_name(ctx, tmp2, sizeof (tmp2));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[1].regnum,
                            src5, sizeof (src5));
    get_ARB1_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER, stage);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "CUBE" : "3D";

    output_line(ctx, "DP3 %s.z, %s, %s;", dst, dst, src4);
    output_line(ctx, "DP3 %s.x, %s, %s;", dst, src0, src1);
    output_line(ctx, "DP3 %s.y, %s, %s;", dst, src2, src3);
    output_line(ctx, "MUL %s, %s, %s;", tmp, dst, dst);    // normal * normal
    output_line(ctx, "MUL %s, %s, %s;", tmp2, dst, src5);  // normal * eyeray

    // !!! FIXME: This is goofy. There's got to be a way to do vector-wide
    // !!! FIXME:  divides or reciprocals...right?
    output_line(ctx, "RCP %s.x, %s.x;", tmp2, tmp2);
    output_line(ctx, "RCP %s.y, %s.y;", tmp2, tmp2);
    output_line(ctx, "RCP %s.z, %s.z;", tmp2, tmp2);
    output_line(ctx, "RCP %s.w, %s.w;", tmp2, tmp2);
    output_line(ctx, "MUL %s, %s, %s;", tmp, tmp, tmp2);

    output_line(ctx, "MUL %s, %s, { 2.0, 2.0, 2.0, 2.0 };", tmp, tmp);
    output_line(ctx, "MAD %s, %s, %s, -%s;", tmp, tmp, dst, src5);
    output_line(ctx, "TEX %s, %s, texture[%d], %s;", dst, tmp, stage, ttypestr);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_TEXM3X3SPEC

static void emit_ARB1_TEXM3X3VSPEC(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];
    char tmp[64];
    char tmp2[64];
    char tmp3[64];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    const int stage = ctx->dest_arg.regnum;
    allocate_ARB1_scratch_reg_name(ctx, tmp, sizeof (tmp));
    allocate_ARB1_scratch_reg_name(ctx, tmp2, sizeof (tmp2));
    allocate_ARB1_scratch_reg_name(ctx, tmp3, sizeof (tmp3));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_ARB1_destarg_varname(ctx, dst, sizeof (dst));

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER, stage);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);
    const char *ttypestr = (ttype == TEXTURE_TYPE_CUBE) ? "CUBE" : "3D";

    output_line(ctx, "MOV %s.x, %s.w;", tmp3, src0);
    output_line(ctx, "MOV %s.y, %s.w;", tmp3, src2);
    output_line(ctx, "MOV %s.z, %s.w;", tmp3, dst);
    output_line(ctx, "DP3 %s.z, %s, %s;", dst, dst, src4);
    output_line(ctx, "DP3 %s.x, %s, %s;", dst, src0, src1);
    output_line(ctx, "DP3 %s.y, %s, %s;", dst, src2, src3);
    output_line(ctx, "MUL %s, %s, %s;", tmp, dst, dst);    // normal * normal
    output_line(ctx, "MUL %s, %s, %s;", tmp2, dst, tmp3);  // normal * eyeray

    // !!! FIXME: This is goofy. There's got to be a way to do vector-wide
    // !!! FIXME:  divides or reciprocals...right?
    output_line(ctx, "RCP %s.x, %s.x;", tmp2, tmp2);
    output_line(ctx, "RCP %s.y, %s.y;", tmp2, tmp2);
    output_line(ctx, "RCP %s.z, %s.z;", tmp2, tmp2);
    output_line(ctx, "RCP %s.w, %s.w;", tmp2, tmp2);
    output_line(ctx, "MUL %s, %s, %s;", tmp, tmp, tmp2);

    output_line(ctx, "MUL %s, %s, { 2.0, 2.0, 2.0, 2.0 };", tmp, tmp);
    output_line(ctx, "MAD %s, %s, %s, -%s;", tmp, tmp, dst, tmp3);
    output_line(ctx, "TEX %s, %s, texture[%d], %s;", dst, tmp, stage, ttypestr);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_TEXM3X3VSPEC

static void emit_ARB1_EXPP(Context *ctx) { emit_ARB1_opcode_ds(ctx, "EX2"); }
static void emit_ARB1_LOGP(Context *ctx) { arb1_log(ctx, "LG2"); }

static void emit_ARB1_CND(Context *ctx)
{
    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_ARB1_srcarg_string(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_ARB1_srcarg_string(ctx, 2, src2, sizeof (src2));
    char tmp[64]; allocate_ARB1_scratch_reg_name(ctx, tmp, sizeof (tmp));

    // CND compares against 0.5, but we need to compare against 0.0...
    //  ...subtract to make up the difference.
    output_line(ctx, "SUB %s, %s, { 0.5, 0.5, 0.5, 0.5 };", tmp, src0);
    // D3D tests (src0 >= 0.0), but ARB1 tests (src0 < 0.0) ... so just
    //  switch src1 and src2 to get the same results.
    output_line(ctx, "CMP%s, %s, %s, %s;", dst, tmp, src2, src1);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_CND

EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2RGB)
EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3TEX)
EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X2DEPTH)
EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3)

static void emit_ARB1_TEXM3X3(Context *ctx)
{
    if (ctx->texm3x3pad_src1 == -1)
        return;

    char dst[64];
    char src0[64];
    char src1[64];
    char src2[64];
    char src3[64];
    char src4[64];

    // !!! FIXME: this code counts on the register not having swizzles, etc.
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst0,
                            src0, sizeof (src0));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src0,
                            src1, sizeof (src1));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_dst1,
                            src2, sizeof (src2));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->texm3x3pad_src1,
                            src3, sizeof (src3));
    get_ARB1_varname_in_buf(ctx, REG_TYPE_TEXTURE, ctx->source_args[0].regnum,
                            src4, sizeof (src4));
    get_ARB1_destarg_varname(ctx, dst, sizeof (dst));

    output_line(ctx, "DP3 %s.z, %s, %s;", dst, dst, src4);
    output_line(ctx, "DP3 %s.x, %s, %s;", dst, src0, src1);
    output_line(ctx, "DP3 %s.y, %s, %s;", dst, src2, src3);
    output_line(ctx, "MOV %s.w, { 1.0, 1.0, 1.0, 1.0 };", dst);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_TEXM3X3

EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXDEPTH)

static void emit_ARB1_CMP(Context *ctx)
{
    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
    char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
    char src1[64]; make_ARB1_srcarg_string(ctx, 1, src1, sizeof (src1));
    char src2[64]; make_ARB1_srcarg_string(ctx, 2, src2, sizeof (src2));
    // D3D tests (src0 >= 0.0), but ARB1 tests (src0 < 0.0) ... so just
    //  switch src1 and src2 to get the same results.
    output_line(ctx, "CMP%s, %s, %s, %s;", dst, src0, src2, src1);
    emit_ARB1_dest_modifiers(ctx);
} // emit_ARB1_CMP

EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(BEM)


static void emit_ARB1_DP2ADD(Context *ctx)
{
    if (support_nv4(ctx))  // nv4 has a built-in equivalent to DP2ADD.
        emit_ARB1_opcode_dsss(ctx, "DP2A");
    else
    {
        char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));
        char src0[64]; make_ARB1_srcarg_string(ctx, 0, src0, sizeof (src0));
        char src1[64]; make_ARB1_srcarg_string(ctx, 1, src1, sizeof (src1));
        char src2[64]; make_ARB1_srcarg_string(ctx, 2, src2, sizeof (src2));
        char scratch[64];

        // DP2ADD is:
        //  dst = (src0.r * src1.r) + (src0.g * src1.g) + src2.replicate_swiz
        allocate_ARB1_scratch_reg_name(ctx, scratch, sizeof (scratch));
        output_line(ctx, "MUL %s, %s, %s;", scratch, src0, src1);
        output_line(ctx, "ADD %s, %s.x, %s.y;", scratch, scratch, scratch);
        output_line(ctx, "ADD%s, %s.x, %s;", dst, scratch, src2);
        emit_ARB1_dest_modifiers(ctx);
    } // else
} // emit_ARB1_DP2ADD


static void emit_ARB1_DSX(Context *ctx)
{
    if (support_nv2(ctx))  // nv2 has a built-in equivalent to DSX.
        emit_ARB1_opcode_ds(ctx, "DDX");
    else
        failf(ctx, "DSX unsupported in %s profile", ctx->profile->name);
} // emit_ARB1_DSX


static void emit_ARB1_DSY(Context *ctx)
{
    if (support_nv2(ctx))  // nv2 has a built-in equivalent to DSY.
        emit_ARB1_opcode_ds(ctx, "DDY");
    else
        failf(ctx, "DSY unsupported in %s profile", ctx->profile->name);
} // emit_ARB1_DSY

static void arb1_texld(Context *ctx, const char *opcode, const int texldd)
{
    // !!! FIXME: Hack: "TEXH" is invalid in nv4. Fix this more cleanly.
    if ((ctx->dest_arg.result_mod & MOD_PP) && (support_nv4(ctx)))
        ctx->dest_arg.result_mod &= ~MOD_PP;

    char dst[64]; make_ARB1_destarg_string(ctx, dst, sizeof (dst));

    const int sm1 = !shader_version_atleast(ctx, 1, 4);
    const int regnum = sm1 ? ctx->dest_arg.regnum : ctx->source_args[1].regnum;
    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER, regnum);

    const char *ttype = NULL;
    char src0[64];
    if (sm1)
        get_ARB1_destarg_varname(ctx, src0, sizeof (src0));
    else
        get_ARB1_srcarg_varname(ctx, 0, src0, sizeof (src0));
    //char src1[64]; get_ARB1_srcarg_varname(ctx, 1, src1, sizeof (src1));  // !!! FIXME: SRC_MOD?

    char src2[64] = { 0 };
    char src3[64] = { 0 };

    if (texldd)
    {
        make_ARB1_srcarg_string(ctx, 2, src2, sizeof (src2));
        make_ARB1_srcarg_string(ctx, 3, src3, sizeof (src3));
    } // if

    // !!! FIXME: this should be in state_TEXLD, not in the arb1/glsl emitters.
    if (sreg == NULL)
    {
        fail(ctx, "TEXLD using undeclared sampler");
        return;
    } // if

    // SM1 only specifies dst, so don't check swizzle there.
    if ( !sm1 && (!no_swizzle(ctx->source_args[1].swizzle)) )
    {
        // !!! FIXME: does this ever actually happen?
        fail(ctx, "BUG: can't handle TEXLD with sampler swizzle at the moment");
    } // if

    switch ((const TextureType) sreg->index)
    {
        case TEXTURE_TYPE_2D: ttype = "2D"; break; // !!! FIXME: "RECT"?
        case TEXTURE_TYPE_CUBE: ttype = "CUBE"; break;
        case TEXTURE_TYPE_VOLUME: ttype = "3D"; break;
        default: fail(ctx, "unknown texture type"); return;
    } // switch

    if (texldd)
    {
        output_line(ctx, "%s%s, %s, %s, %s, texture[%d], %s;", opcode, dst,
                    src0, src2, src3, regnum, ttype);
    } // if
    else
    {
        output_line(ctx, "%s%s, %s, texture[%d], %s;", opcode, dst, src0,
                    regnum, ttype);
    } // else
} // arb1_texld


static void emit_ARB1_TEXLDD(Context *ctx)
{
    // With GL_NV_fragment_program2, we can use the TXD opcode.
    //  In stock arb1, we can settle for a standard texld, which isn't
    //  perfect, but oh well.
    if (support_nv2(ctx))
        arb1_texld(ctx, "TXD", 1);
    else
        arb1_texld(ctx, "TEX", 0);
} // emit_ARB1_TEXLDD


static void emit_ARB1_TEXLDL(Context *ctx)
{
    if ((shader_is_vertex(ctx)) && (!support_nv3(ctx)))
    {
        failf(ctx, "Vertex shader TEXLDL unsupported in %s profile",
              ctx->profile->name);
        return;
    } // if

    else if ((shader_is_pixel(ctx)) && (!support_nv2(ctx)))
    {
        failf(ctx, "Pixel shader TEXLDL unsupported in %s profile",
              ctx->profile->name);
        return;
    } // if

    // !!! FIXME: this doesn't map exactly to TEXLDL. Review this.
    arb1_texld(ctx, "TXL", 0);
} // emit_ARB1_TEXLDL


EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(BREAKP)
EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(BREAKC)

static void emit_ARB1_IFC(Context *ctx)
{
    if (support_nv2(ctx))
    {
        static const char *comps[] = {
            "", "SGTC", "SEQC", "SGEC", "SGTC", "SNEC", "SLEC"
        };

        if (ctx->instruction_controls >= STATICARRAYLEN(comps))
        {
            fail(ctx, "unknown comparison control");
            return;
        } // if

        char src0[64];
        char src1[64];
        char scratch[64];

        const char *comp = comps[ctx->instruction_controls];
        get_ARB1_srcarg_varname(ctx, 0, src0, sizeof (src0));
        get_ARB1_srcarg_varname(ctx, 1, src1, sizeof (src1));
        allocate_ARB1_scratch_reg_name(ctx, scratch, sizeof (scratch));
        output_line(ctx, "%s %s.x, %s, %s;", comp, scratch, src0, src1);
        nv2_if(ctx);
    } // if

    else  // stock ARB1 has no branching.
    {
        failf(ctx, "branching unsupported in %s profile", ctx->profile->name);
    } // else
} // emit_ARB1_IFC


EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(SETP)

static void emit_ARB1_DEF(Context *ctx)
{
    const float *val = (const float *) ctx->dwords; // !!! FIXME: could be int?
    char dst[64]; get_ARB1_destarg_varname(ctx, dst, sizeof (dst));
    char val0[32]; floatstr(ctx, val0, sizeof (val0), val[0], 1);
    char val1[32]; floatstr(ctx, val1, sizeof (val1), val[1], 1);
    char val2[32]; floatstr(ctx, val2, sizeof (val2), val[2], 1);
    char val3[32]; floatstr(ctx, val3, sizeof (val3), val[3], 1);

    push_output(ctx, &ctx->globals);
    output_line(ctx, "PARAM %s = { %s, %s, %s, %s };",
                dst, val0, val1, val2, val3);
    pop_output(ctx);
} // emit_ARB1_DEF

static void emit_ARB1_DEFI(Context *ctx)
{
    char dst[64]; get_ARB1_destarg_varname(ctx, dst, sizeof (dst));
    const int32 *x = (const int32 *) ctx->dwords;
    push_output(ctx, &ctx->globals);
    output_line(ctx, "PARAM %s = { %d, %d, %d, %d };",
                dst, (int) x[0], (int) x[1], (int) x[2], (int) x[3]);
    pop_output(ctx);
} // emit_ARB1_DEFI

static void emit_ARB1_DEFB(Context *ctx)
{
    char dst[64]; get_ARB1_destarg_varname(ctx, dst, sizeof (dst));
    push_output(ctx, &ctx->globals);
    output_line(ctx, "PARAM %s = %d;", dst, ctx->dwords[0] ? 1 : 0);
    pop_output(ctx);
} // emit_ARB1_DEFB

static void emit_ARB1_DCL(Context *ctx)
{
    // no-op. We do this in our emit_attribute() and emit_uniform().
} // emit_ARB1_DCL

EMIT_ARB1_OPCODE_UNIMPLEMENTED_FUNC(TEXCRD)

static void emit_ARB1_TEXLD(Context *ctx)
{
    if (!shader_version_atleast(ctx, 1, 4))
    {
        arb1_texld(ctx, "TEX", 0);
        return;
    } // if

    else if (!shader_version_atleast(ctx, 2, 0))
    {
        // ps_1_4 is different, too!
        fail(ctx, "TEXLD == Shader Model 1.4 unimplemented.");  // !!! FIXME
        return;
    } // if

    // !!! FIXME: do texldb and texldp map between OpenGL and D3D correctly?
    if (ctx->instruction_controls == CONTROL_TEXLD)
        arb1_texld(ctx, "TEX", 0);
    else if (ctx->instruction_controls == CONTROL_TEXLDP)
        arb1_texld(ctx, "TXP", 0);
    else if (ctx->instruction_controls == CONTROL_TEXLDB)
        arb1_texld(ctx, "TXB", 0);
} // emit_ARB1_TEXLD

#endif  // SUPPORT_PROFILE_ARB1

#if !SUPPORT_PROFILE_SPIRV
#define PROFILE_EMITTER_SPIRV(op)
#else
#undef AT_LEAST_ONE_PROFILE
#define AT_LEAST_ONE_PROFILE 1
#define PROFILE_EMITTER_SPIRV(op) emit_SPIRV_##op,

#define EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(op) \
    static void emit_SPIRV_##op(Context *ctx) { \
        /* fail(ctx, #op " unimplemented in spirv profile"); */ \
        fprintf(stderr, "%s\n", #op " unimplemented in spirv profile"); \
    }

#include "spirv/spirv.h"
#include "spirv/GLSL.std.450.h"

static uint32 spv_bumpid(Context *ctx)
{
    return (ctx->spirv.idmax += 1);
} // spv_bumpid

static RegisterList *spv_getreg(Context *ctx, const RegisterType regtype, const int regnum)
{
    RegisterList *r = reglist_find(&ctx->used_registers, regtype, regnum);
    if (!r) {
        failf(ctx, "register not found rt=%d, rn=%d", regtype, regnum);
        return NULL;
    }
    return r;
} // spv_getreg

static void componentlist_free(Context *ctx, ComponentList *cl)
{
    ComponentList *next;
    while (cl)
    {
        next = cl->next;
        Free(ctx, cl);
        cl = next;
    } // while
} // componentlist_free

static ComponentList *componentlist_alloc(Context *ctx)
{
    ComponentList *ret = (ComponentList *) Malloc(ctx, sizeof(ComponentList));
    if (!ret) return NULL;
    ret->id = 0;
    ret->v.i = 0;
    ret->next = NULL;
    return ret;
} // componentlist_alloc

static const char *get_SPIRV_varname_in_buf(Context *ctx, const RegisterType rt,
                                           const int regnum, char *buf,
                                           const size_t buflen)
{
    // turns out these are identical at the moment.
    return get_D3D_varname_in_buf(ctx, rt, regnum, buf, buflen);
} // get_SPIRV_varname_in_buf

static const char *get_SPIRV_varname(Context *ctx, const RegisterType rt,
                                    const int regnum)
{
    // turns out these are identical at the moment.
    return get_D3D_varname(ctx, rt, regnum);
} // get_SPIRV_varname


static inline const char *get_SPIRV_const_array_varname_in_buf(Context *ctx,
                                                const int base, const int size,
                                                char *buf, const size_t buflen)
{
    snprintf(buf, buflen, "c_array_%d_%d", base, size);
    return buf;
} // get_SPIRV_const_array_varname_in_buf


static const char *get_SPIRV_const_array_varname(Context *ctx, int base, int size)
{
    char buf[64];
    get_SPIRV_const_array_varname_in_buf(ctx, base, size, buf, sizeof (buf));
    return StrDup(ctx, buf);
} // get_SPIRV_const_array_varname


static void output_u32(Context *ctx, uint32 word)
{
    assert(ctx->output != NULL);
    if (isfail(ctx))
        return;  // we failed previously, don't go on...

    buffer_append(ctx->output, &word, sizeof(word));
} // output_u32

// Helper for debugging purposes, checks SSA id is not 0
static void output_id(Context *ctx, uint32 id)
{
    if (id == 0) {
        assert(0);
    }
    output_u32(ctx, id);
} // output_id

// (len) total op length in words, includes opcode + all params
static void output_spvop(Context *ctx, uint32 op, uint32 len)
{
    assert(ctx->output != NULL);
    if (isfail(ctx))
        return;  // we failed previously, don't go on...

    output_u32(ctx, op | (len << 16));
} // output_spvop

static void output_spvstr(Context *ctx, const char *str)
{
    size_t len;
    uint32 trail;
    assert(ctx->output != NULL);
    if (isfail(ctx))
        return;  // we failed previously, don't go on...

    if (str == NULL) {
        return output_u32(ctx, 0);
    }
    len = strlen(str) + 1;
    buffer_append(ctx->output, str, len);
    len = len % 4;
    if (len) {
        trail = 0;
        buffer_append(ctx->output, &trail, 4 - len);
    }
} // output_spvstr

// get the word count of a string
static uint32 spv_strlen(const char *str)
{
    size_t len = strlen(str);
    return (uint32) (
        len / 4 + 1
    );
} // spv_strlen

// emits an OpName straight into ctx->globals
static void output_spvname(Context *ctx, uint32 id, const char *str)
{
    if (isfail(ctx))
        return;  // we failed previously, don't go on...

    push_output(ctx, &ctx->globals);
    output_spvop(ctx, SpvOpName, 2 + spv_strlen(str));
    output_u32(ctx, id);
    output_spvstr(ctx, str);
    pop_output(ctx);
} // output_spvname

// emits an OpDecorate BuiltIn straight into ctx->helpers
static void output_spvbuiltin(Context *ctx, uint32 id, SpvBuiltIn builtin)
{
    if (isfail(ctx))
        return;  // we failed previously, don't go on...

    push_output(ctx, &ctx->helpers);
    output_spvop(ctx, SpvOpDecorate, 4);
    output_u32(ctx, id);
    output_u32(ctx, SpvDecorationBuiltIn);
    output_u32(ctx, builtin);
    pop_output(ctx);
} // output_spvbuiltin

static uint32 spv_getvoid(Context *ctx)
{
    uint32 id;
    if (ctx->spirv.types.idvoid)
    {
        return ctx->spirv.types.idvoid;
    } // if

    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeVoid, 2);
    output_u32(ctx, id);
    pop_output(ctx);
    return ctx->spirv.types.idvoid = id;
} // spv_getvoid

static uint32 spv_getfuncv(Context *ctx)
{
    uint32 id, vid;
    if (ctx->spirv.types.idfuncv)
    {
        return ctx->spirv.types.idfuncv;
    } // if

    vid = spv_getvoid(ctx);
    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeFunction, 3);
    output_u32(ctx, id);
    output_u32(ctx, vid);
    pop_output(ctx);
    return ctx->spirv.types.idfuncv = id;
} // spv_getfuncv

static uint32 spv_getbool(Context *ctx)
{
    uint32 id;
    if (ctx->spirv.types.idbool)
    {
        return ctx->spirv.types.idbool;
    } // if

    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeBool, 2);
    output_u32(ctx, id);
    pop_output(ctx);
    return ctx->spirv.types.idbool = id;
} // spv_getbool

static uint32 spv_gettrue(Context *ctx)
{
    uint32 id, bid;
    if (ctx->spirv.types.idtrue)
    {
        return ctx->spirv.types.idtrue;
    } // if

    bid = spv_getbool(ctx);
    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpConstantTrue, 3);
    output_u32(ctx, bid);
    output_u32(ctx, id);
    pop_output(ctx);
    return ctx->spirv.types.idtrue = id;
} // spv_gettrue

static uint32 spv_getfalse(Context *ctx)
{
    uint32 id, bid;
    if (ctx->spirv.types.idfalse)
    {
        return ctx->spirv.types.idfalse;
    } // if

    bid = spv_getbool(ctx);
    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpConstantFalse, 3);
    output_u32(ctx, bid);
    output_u32(ctx, id);
    pop_output(ctx);
    return ctx->spirv.types.idfalse = id;
} // spv_getfalse

static uint32 spv_getfloat(Context *ctx)
{
    uint32 id;
    if (ctx->spirv.types.idfloat)
    {
        return ctx->spirv.types.idfloat;
    } // if

    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeFloat, 3);
    output_u32(ctx, id);
    output_u32(ctx, 32);
    pop_output(ctx);
    return ctx->spirv.types.idfloat = id;
} // spv_getfloat

static uint32 spv_getint(Context *ctx)
{
    uint32 id;
    if (ctx->spirv.types.idint)
    {
        return ctx->spirv.types.idint;
    } // if

    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeInt, 4);
    output_u32(ctx, id);
    output_u32(ctx, 32);
    output_u32(ctx, 1);
    pop_output(ctx);
    return ctx->spirv.types.idint = id;
} // spv_getint

static uint32 spv_getuint(Context *ctx)
{
    uint32 id;
    if (ctx->spirv.types.iduint)
    {
        return ctx->spirv.types.iduint;
    } // if

    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeInt, 4);
    output_u32(ctx, id);
    output_u32(ctx, 32);
    output_u32(ctx, 0);
    pop_output(ctx);
    return ctx->spirv.types.iduint = id;
} // spv_getint

static uint32 spv_getvec4(Context *ctx)
{
    uint32 id, fid;
    if (ctx->spirv.types.idvec4)
    {
        return ctx->spirv.types.idvec4;
    } // if

    fid = spv_getfloat(ctx);
    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeVector, 4);
    output_u32(ctx, id);
    output_u32(ctx, fid);
    output_u32(ctx, 4);
    pop_output(ctx);
    return ctx->spirv.types.idvec4 = id;
} // spv_getvec4

static uint32 spv_getivec4(Context *ctx)
{
    uint32 id, iid;
    if (ctx->spirv.types.idivec4)
    {
        return ctx->spirv.types.idivec4;
    } // if

    iid = spv_getint(ctx);
    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeVector, 4);
    output_u32(ctx, id);
    output_u32(ctx, iid);
    output_u32(ctx, 4);
    pop_output(ctx);
    return ctx->spirv.types.idivec4 = id;
} // spv_getvec4i

static uint32 spv_getvec3(Context *ctx)
{
    uint32 id, fid;
    if (ctx->spirv.types.idvec3)
    {
        return ctx->spirv.types.idvec3;
    } // if

    fid = spv_getfloat(ctx);
    id = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpTypeVector, 4);
    output_u32(ctx, id);
    output_u32(ctx, fid);
    output_u32(ctx, 3);
    pop_output(ctx);
    return ctx->spirv.types.idvec3 = id;
} // spv_getvec4

static uint32 _spv_getimage(Context *ctx, uint32 *cached, SpvDim dim)
{
    if (*cached != 0)
    {
        return *cached;
    } // if

    uint32 image_id = spv_bumpid(ctx);
    uint32 sampled_image_id = spv_bumpid(ctx);
    uint32 fid = spv_getfloat(ctx);

    push_output(ctx, &ctx->mainline_intro);

    output_spvop(ctx, SpvOpTypeImage, 9);
    output_id(ctx, image_id); // result id
    output_id(ctx, fid); // sampled type id
    output_u32(ctx, dim); // texture dim
    output_u32(ctx, 0); // not a depth image
    output_u32(ctx, 0); // non-arrayed content
    output_u32(ctx, 0); // no multi-sampling
    output_u32(ctx, 1); // this will be used with a sampler
    output_u32(ctx, SpvImageFormatUnknown);

    output_spvop(ctx, SpvOpTypeSampledImage, 3);
    output_id(ctx, sampled_image_id);
    output_id(ctx, image_id);

    pop_output(ctx);

    *cached = sampled_image_id;
    return sampled_image_id;
} // _spv_getimage

static uint32 spv_getimage2d(Context *ctx)
{
    return _spv_getimage(ctx, &ctx->spirv.types.idimage2d, SpvDim2D);
} // spv_getimage2d

static uint32 spv_getimage3d(Context *ctx)
{
    return _spv_getimage(ctx, &ctx->spirv.types.idimage3d, SpvDim3D);
} // spv_getimage2d

static uint32 spv_getimagecube(Context *ctx)
{
    return _spv_getimage(ctx, &ctx->spirv.types.idimagecube, SpvDimCube);
} // spv_getimage2d

#define SPV_MAKE_GETPTR(_var, _from, _storageclass) \
    static uint32 spv_get ## _var(Context *ctx) \
    { \
        uint32 id, fid; \
        if (ctx->spirv.types.id ## _var) { \
            return ctx->spirv.types.id ## _var; \
        } \
        fid = spv_get ## _from(ctx); \
        id = spv_bumpid(ctx); \
        push_output(ctx, &ctx->mainline_intro); \
        output_spvop(ctx, SpvOpTypePointer, 4); \
        output_u32(ctx, id); \
        output_u32(ctx, SpvStorageClass ## _storageclass); \
        output_u32(ctx, fid); \
        pop_output(ctx); \
        return ctx->spirv.types.id ## _var = id; \
    }

SPV_MAKE_GETPTR(ptrvec4u, vec4, Uniform);
SPV_MAKE_GETPTR(ptrivec4u, ivec4, Uniform);
SPV_MAKE_GETPTR(ptrvec4i, vec4, Input);
SPV_MAKE_GETPTR(ptrivec4i, ivec4, Input);
SPV_MAKE_GETPTR(ptrvec4o, vec4, Output);
SPV_MAKE_GETPTR(ptrivec4o, ivec4, Output);
SPV_MAKE_GETPTR(ptrvec4p, vec4, Private);
SPV_MAKE_GETPTR(ptrivec4p, ivec4, Private);

SPV_MAKE_GETPTR(ptrfloato, float, Output);

SPV_MAKE_GETPTR(ptrimage2d, image2d, Uniform);
SPV_MAKE_GETPTR(ptrimage3d, image3d, Uniform);
SPV_MAKE_GETPTR(ptrimagecube, imagecube, Uniform);

#undef SPV_MAKE_GETPTR

static uint32 spv_getext(Context *ctx)
{
    if (ctx->spirv.idext)
    {
        return ctx->spirv.idext;
    } // if

    return ctx->spirv.idext = spv_bumpid(ctx);
} // spv_getext

static uint32 spv_emitscalar(Context *ctx, ComponentList *cl,
                             MOJOSHADER_attributeType type)
{
    uint32 idret, idtype;
    if (type == MOJOSHADER_ATTRIBUTE_FLOAT)
    {
        idtype = spv_getfloat(ctx);
    } // if
    else if (type == MOJOSHADER_ATTRIBUTE_INT)
    {
        idtype = spv_getint(ctx);
    } // else if
    else if (type == MOJOSHADER_ATTRIBUTE_UINT)
    {
        idtype = spv_getuint(ctx);
    } // else if
    else
    {
        failf(ctx, "%s: invalid attribute type %d", __func__, type);
        return 0;
    }
    idret = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpConstant, 4);
    output_u32(ctx, idtype);
    output_u32(ctx, idret);
    output_u32(ctx, cl->v.u);
    pop_output(ctx);
    return idret;
} // spv_emitscalar

// The spv_getscalar* functions retrieve the result id of an OpConstant
// instruction with the corresponding value v, or generate a new one.
static uint32 spv_getscalarf(Context *ctx, float v)
{
    ComponentList *prev = &(ctx->spirv.cl.f), *cl = ctx->spirv.cl.f.next;
    while (cl)
    {
        if (v == cl->v.f)
        {
            return cl->id;
        } // if
        else if (v < cl->v.f)
        {
            break;
        } // else if
        prev = cl;
        cl = cl->next;
    } // while
    cl = componentlist_alloc(ctx);
    cl->next = prev->next;
    prev->next = cl;
    cl->v.f = v;
    cl->id = spv_emitscalar(ctx, cl, MOJOSHADER_ATTRIBUTE_FLOAT);
    return cl->id;
} // spv_getscalarf

static uint32 spv_getscalari(Context *ctx, int v)
{
    ComponentList *prev = &(ctx->spirv.cl.i), *cl = ctx->spirv.cl.i.next;
    while (cl)
    {
        if (v == cl->v.i)
        {
            return cl->id;
        } // if
        else if (v < cl->v.i)
        {
            break;
        } // else if
        prev = cl;
        cl = cl->next;
    } // while
    cl = componentlist_alloc(ctx);
    cl->next = prev->next;
    prev->next = cl;
    cl->v.i = v;
    cl->id = spv_emitscalar(ctx, cl, MOJOSHADER_ATTRIBUTE_INT);
    return cl->id;
} // spv_getscalari

static uint32 spv_getscalaru(Context *ctx, uint32 v)
{
    ComponentList *prev = &(ctx->spirv.cl.u), *cl = ctx->spirv.cl.u.next;
    while (cl)
    {
        if (v == cl->v.u)
        {
            return cl->id;
        } // if
        else if (v < cl->v.u)
        {
            break;
        } // else if
        prev = cl;
        cl = cl->next;
    } // while
    cl = componentlist_alloc(ctx);
    cl->next = prev->next;
    prev->next = cl;
    cl->v.u = v;
    cl->id = spv_emitscalar(ctx, cl, MOJOSHADER_ATTRIBUTE_UINT);
    return cl->id;
} // spv_getscalaru

static void spv_check_read_reg_id(Context *ctx, RegisterList *r)
{
    if (r->spirv.iddecl == 0)
    {
        switch (r->regtype)
        {
            case REG_TYPE_INPUT: // v#
            {
                char varname[64];
                get_SPIRV_varname_in_buf(ctx, r->regtype, r->regnum, varname, sizeof(varname));
                failf(ctx, "tried to load from undeclared register %s\n", varname);
                break;
            }

            case REG_TYPE_TEMP: // r#
            case REG_TYPE_CONST: // c#
            case REG_TYPE_CONSTINT: // i#
            case REG_TYPE_CONSTBOOL: // b#
                r->spirv.iddecl = spv_bumpid(ctx);
                break;

            default:
            {
                char varname[64];
                get_SPIRV_varname_in_buf(ctx, r->regtype, r->regnum, varname, sizeof(varname));
                failf(ctx, "register type %s is unimplemented\n", varname);
                break;
            }
        } // switch
    } // if
}

static void spv_check_write_reg_id(Context *ctx, RegisterList *r)
{
    if (r->spirv.iddecl == 0)
    {
        switch (r->regtype)
        {
            // These registers require no declarations, so we can just create them as we see them
            case REG_TYPE_TEMP:
            case REG_TYPE_RASTOUT:
            case REG_TYPE_COLOROUT:
            case REG_TYPE_TEXCRDOUT:
            case REG_TYPE_DEPTHOUT:
            case REG_TYPE_ATTROUT:
                r->spirv.iddecl = spv_bumpid(ctx);
                break;

            // Other register types should be explicitly declared, so it is an error for them to have iddecl == 0 by now
            default:
            {
                char varname[64];
                get_SPIRV_varname_in_buf(ctx, r->regtype, r->regnum, varname, sizeof(varname));
                failf(ctx, "tried to write to undeclared register %s\n", varname);
                break;
            }
        } // switch
    } // if
}

static uint32 spv_loadreg(Context *ctx, RegisterList *r)
{
    const RegisterType regtype = r->regtype;
    const int regnum = r->regnum;

    uint32 tid = spv_getvec4(ctx);

    spv_check_read_reg_id(ctx, r);

    uint32 result = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline);
    output_spvop(ctx, SpvOpLoad, 4);
    output_id(ctx, tid);
    output_id(ctx, result);
    output_id(ctx, r->spirv.iddecl);
    pop_output(ctx);

    return result;
} // spv_loadreg

uint32 spv_swizzle(Context *ctx, uint32 arg, const int swizzle,
                   const int writemask)
{
    // Nothing to do, so return the same SSA value
    if (no_swizzle(swizzle) && writemask_xyzw(writemask)) {
        return arg;
    } // if

    uint32 result = spv_bumpid(ctx);

    const int writemask0 = (writemask >> 0) & 0x1;
    const int writemask1 = (writemask >> 1) & 0x1;
    const int writemask2 = (writemask >> 2) & 0x1;
    const int writemask3 = (writemask >> 3) & 0x1;

    const uint32 swizzle_x = (swizzle >> 0) & 0x3;
    const uint32 swizzle_y = (swizzle >> 2) & 0x3;
    const uint32 swizzle_z = (swizzle >> 4) & 0x3;
    const uint32 swizzle_w = (swizzle >> 6) & 0x3;

    uint32 vec4 = spv_getvec4(ctx);

    push_output(ctx, &ctx->mainline);
    output_spvop(ctx, SpvOpVectorShuffle, 5 + 4);
    output_u32(ctx, vec4);
    output_u32(ctx, result);
    // OpVectorShuffle takes two vectors to shuffle, but to do a swizzle
    // operation we can just ignore the second argument (meaning it can be
    // anything, and I am just making it `arg` for convenience)
    output_u32(ctx, arg);
    output_u32(ctx, arg);

    int i = 0;
    if (writemask0) { output_u32(ctx, swizzle_x); i++; }
    if (writemask1) { output_u32(ctx, swizzle_y); i++; }
    if (writemask2) { output_u32(ctx, swizzle_z); i++; }
    if (writemask3) { output_u32(ctx, swizzle_w); i++; }

    // All remaining components are left undefined, indicated by a value of 0xFFFFFFFF
    for (; i < 4; i++) {
        output_u32(ctx, 0xFFFFFFFF);
    }

    pop_output(ctx);

    return result;
} // make_GLSL_swizzle_string

static uint32 spv_load_srcarg(Context *ctx, const size_t idx, const int writemask)
{
    if (idx >= STATICARRAYLEN(ctx->source_args))
    {
        fail(ctx, "Too many source args");
        return 0;
    } // if

    const SourceArgInfo *arg = &ctx->source_args[idx];

    RegisterList *reg = spv_getreg(ctx, arg->regtype, arg->regnum);
    uint32 result = spv_loadreg(ctx, reg);

    result = spv_swizzle(ctx, result, arg->swizzle, writemask);

//    const char *premod_str = "";
//    const char *postmod_str = "";
    switch (arg->src_mod)
    {
        case SRCMOD_NEGATE:
        {
            uint32 new_result = spv_bumpid(ctx);
            uint32 vec4 = spv_getvec4(ctx);
            push_output(ctx, &ctx->mainline);
            output_spvop(ctx, SpvOpFNegate, 4);
            output_id(ctx, vec4);
            output_id(ctx, new_result);
            output_id(ctx, result);
            pop_output(ctx);
            result = new_result;
            break;
        }

        default:
            failf(ctx, "unsupported source modifier %d", arg->src_mod);
            return 0;

//        case SRCMOD_BIASNEGATE:
//            premod_str = "-(";
//            postmod_str = " - 0.5)";
//            break;
//
//        case SRCMOD_BIAS:
//            premod_str = "(";
//            postmod_str = " - 0.5)";
//            break;
//
//        case SRCMOD_SIGNNEGATE:
//            premod_str = "-((";
//            postmod_str = " - 0.5) * 2.0)";
//            break;
//
//        case SRCMOD_SIGN:
//            premod_str = "((";
//            postmod_str = " - 0.5) * 2.0)";
//            break;
//
//        case SRCMOD_COMPLEMENT:
//            premod_str = "(1.0 - ";
//            postmod_str = ")";
//            break;
//
//        case SRCMOD_X2NEGATE:
//            premod_str = "-(";
//            postmod_str = " * 2.0)";
//            break;
//
//        case SRCMOD_X2:
//            premod_str = "(";
//            postmod_str = " * 2.0)";
//            break;
//
//        case SRCMOD_DZ:
//            fail(ctx, "SRCMOD_DZ unsupported"); return buf; // !!! FIXME
//            postmod_str = "_dz";
//            break;
//
//        case SRCMOD_DW:
//            fail(ctx, "SRCMOD_DW unsupported"); return buf; // !!! FIXME
//            postmod_str = "_dw";
//            break;
//
//        case SRCMOD_ABSNEGATE:
//            premod_str = "-abs(";
//            postmod_str = ")";
//            break;
//
//        case SRCMOD_ABS:
//            premod_str = "abs(";
//            postmod_str = ")";
//            break;
//
//        case SRCMOD_NOT:
//            premod_str = "!";
//            break;

        case SRCMOD_NONE:
        case SRCMOD_TOTAL:
            break;  // stop compiler whining.
    } // switch

//    const char *regtype_str = NULL;

//    if (!arg->relative)
//    {
//        regtype_str = get_GLSL_varname_in_buf(ctx, arg->regtype, arg->regnum,
//                                              (char *) alloca(64), 64);
//    } // if

//    const char *rel_lbracket = "";
//    char rel_offset[32] = { '\0' };
//    const char *rel_rbracket = "";
//    char rel_swizzle[4] = { '\0' };
//    const char *rel_regtype_str = "";
    if (arg->relative)
    {
        fail(ctx, "relative register access is unimplemented");
//        if (arg->regtype == REG_TYPE_INPUT)
//            regtype_str=get_GLSL_input_array_varname(ctx,(char*)alloca(64),64);
//        else
//        {
//            assert(arg->regtype == REG_TYPE_CONST);
//            const int arrayidx = arg->relative_array->index;
//            const int offset = arg->regnum - arrayidx;
//            assert(offset >= 0);
//            if (arg->relative_array->constant)
//            {
//                const int arraysize = arg->relative_array->count;
//                regtype_str = get_GLSL_const_array_varname_in_buf(ctx,
//                                                                  arrayidx, arraysize, (char *) alloca(64), 64);
//                if (offset != 0)
//                    snprintf(rel_offset, sizeof (rel_offset), "%d + ", offset);
//            } // if
//            else
//            {
//                regtype_str = get_GLSL_uniform_array_varname(ctx, arg->regtype,
//                                                             (char *) alloca(64), 64);
//                if (offset == 0)
//                {
//                    snprintf(rel_offset, sizeof (rel_offset),
//                             "ARRAYBASE_%d + ", arrayidx);
//                } // if
//                else
//                {
//                    snprintf(rel_offset, sizeof (rel_offset),
//                             "(ARRAYBASE_%d + %d) + ", arrayidx, offset);
//                } // else
//            } // else
//        } // else
//
//        rel_lbracket = "[";
//
//        rel_regtype_str = get_GLSL_varname_in_buf(ctx, arg->relative_regtype,
//                                                  arg->relative_regnum,
//                                                  (char *) alloca(64), 64);
//        rel_swizzle[0] = '.';
//        rel_swizzle[1] = swizzle_channels[arg->relative_component];
//        rel_swizzle[2] = '\0';
//        rel_rbracket = "]";
    } // if

//    snprintf(buf, buflen, "%s%s%s%s%s%s%s%s%s",
//             premod_str, regtype_str, rel_lbracket, rel_offset,
//             rel_regtype_str, rel_swizzle, rel_rbracket, swiz_str,
//             postmod_str);
    // !!! FIXME: make sure the scratch buffer was large enough.
    return result;
} // spv_load_srcarg

// generate some convenience functions.
#define MAKE_GLSL_SRCARG_STRING_(mask, bitmask) \
    static inline uint32 spv_load_srcarg_##mask(Context *ctx, \
                                                const size_t idx) { \
        return spv_load_srcarg(ctx, idx, bitmask); \
    }
MAKE_GLSL_SRCARG_STRING_(x, (1 << 0))
MAKE_GLSL_SRCARG_STRING_(y, (1 << 1))
MAKE_GLSL_SRCARG_STRING_(z, (1 << 2))
MAKE_GLSL_SRCARG_STRING_(w, (1 << 3))
MAKE_GLSL_SRCARG_STRING_(scalar, (1 << 0))
MAKE_GLSL_SRCARG_STRING_(full, 0xF)
MAKE_GLSL_SRCARG_STRING_(masked, ctx->dest_arg.writemask)
MAKE_GLSL_SRCARG_STRING_(vec3, 0x7)
MAKE_GLSL_SRCARG_STRING_(vec2, 0x3)
#undef MAKE_GLSL_SRCARG_STRING_

static void spv_assign_destarg(Context *ctx, uint32 value)
{
    const DestArgInfo *arg = &ctx->dest_arg;
    RegisterList *reg = spv_getreg(ctx, arg->regtype, arg->regnum);

    spv_check_write_reg_id(ctx, reg);

    if (arg->writemask == 0)
    {
        // Return without updating the reg->spirv.iddecl (all-zero writemask = no-op)
        return;
    } // if

    if (arg->result_mod & MOD_SATURATE)
    {
        // TODO: The SPIRV translation of saturate(x) should look like:
        // rd = Select(FUnordLessThan(rd, 0.0), 0.0, rd)
        // rd = Select(FUnordGreaterThan(rd, 1.0), 1.0, rd)

        fail(ctx, "saturating dest arg is unimplemented");
    } // if

    // MSDN says MOD_PP is a hint and many implementations ignore it. So do we.

    // CENTROID only allowed in DCL opcodes, which shouldn't come through here.
    assert((arg->result_mod & MOD_CENTROID) == 0);

    if (ctx->predicated)
    {
        fail(ctx, "predicated destinations unsupported");  // !!! FIXME
        return;
    } // if

    switch (arg->result_shift)
    {
        case 0x1: // result_shift_str = " * 2.0"; break;
        case 0x2: // result_shift_str = " * 4.0"; break;
        case 0x3: // result_shift_str = " * 8.0"; break;
        case 0xD: // result_shift_str = " / 8.0"; break;
        case 0xE: // result_shift_str = " / 4.0"; break;
        case 0xF: // result_shift_str = " / 2.0"; break;
            failf(ctx, "result shift %d not implemented", arg->result_shift);
    } // switch

    if (!writemask_xyzw(arg->writemask))
    {
        uint32 vec4 = spv_getvec4(ctx);
        uint32 new_value = spv_bumpid(ctx);
        uint32 current_value = spv_bumpid(ctx);

        push_output(ctx, &ctx->mainline);

        output_spvop(ctx, SpvOpLoad, 4);
        output_id(ctx, vec4);
        output_id(ctx, current_value);
        output_id(ctx, reg->spirv.iddecl);

        output_spvop(ctx, SpvOpVectorShuffle, 5 + 4);
        output_id(ctx, vec4);
        output_id(ctx, new_value); // output id is new_value
        // select between current value and new value based on writemask
        output_id(ctx, value);
        output_id(ctx, current_value);

        // in the shuffle, components [0, 3] are the new value, and components
        // [4, 7] are the existing value
        if (arg->writemask0) output_u32(ctx, 0); else output_u32(ctx, 4);
        if (arg->writemask1) output_u32(ctx, 1); else output_u32(ctx, 5);
        if (arg->writemask2) output_u32(ctx, 2); else output_u32(ctx, 6);
        if (arg->writemask3) output_u32(ctx, 3); else output_u32(ctx, 7);

        pop_output(ctx);

        value = new_value;
    } // if

    switch (reg->regtype) {
        case REG_TYPE_OUTPUT:
        case REG_TYPE_ADDRESS:
        case REG_TYPE_TEMP:
        case REG_TYPE_DEPTHOUT:
        case REG_TYPE_COLOROUT:
        case REG_TYPE_RASTOUT:
        case REG_TYPE_ATTROUT:
            push_output(ctx, &ctx->mainline);
            output_spvop(ctx, SpvOpStore, 3);
            output_id(ctx, reg->spirv.iddecl);
            output_u32(ctx, value);
            pop_output(ctx);
            break;

        default:
        {
            char varname[64];
            get_SPIRV_varname_in_buf(ctx, reg->regtype, reg->regnum, varname, sizeof(varname));
            failf(ctx, "register %s is unimplemented for storing", varname);
            break;
        }
    }
}

static void emit_SPIRV_start(Context *ctx, const char *profilestr)
{
    if (!(
        shader_is_vertex(ctx) ||
        shader_is_pixel(ctx)
    ))
    {
        failf(ctx, "Shader type %u unsupported in this profile.",
              (uint) ctx->shader_type);
        return;
    } // if

    if (strcmp(profilestr, MOJOSHADER_PROFILE_SPIRV) != 0)
    {
        failf(ctx, "Profile '%s' unsupported or unknown.", profilestr);
    } // if

    memset(&(ctx->spirv), '\0', sizeof(ctx->spirv));

    ctx->spirv.idmain = spv_bumpid(ctx);

    // calls spv_getvoid as well
    spv_getfuncv(ctx);

    // slap the function declaration itself in mainline_top, so we can do type
    // declaration in mainline_intro (= before this in the output)
    push_output(ctx, &ctx->mainline_top);

    output_spvop(ctx, SpvOpFunction, 5);
    output_u32(ctx, spv_getvoid(ctx));
    output_u32(ctx, ctx->spirv.idmain);
    output_u32(ctx, SpvFunctionControlMaskNone);
    output_u32(ctx, spv_getfuncv(ctx));

    output_spvop(ctx, SpvOpLabel, 2);
    output_u32(ctx, spv_bumpid(ctx));

    pop_output(ctx);

    // also emit the name for the function
    output_spvname(ctx, ctx->spirv.idmain, ctx->mainfn);

    set_output(ctx, &ctx->mainline);
} // emit_SPIRV_start

static void emit_SPIRV_end(Context *ctx)
{
} // emit_SPIRV_end

static void emit_SPIRV_phase(Context *ctx){
    // no-op
} // emit_SPIRV_phase

static void emit_SPIRV_global(Context *ctx, RegisterType regtype, int regnum)
{
    RegisterList *r = reglist_find(&ctx->used_registers, regtype, regnum);

    // TODO: If the SSA id for this register is still 0 by this point, that means no instructions actually
    // loaded from/stored to this variable...

    if (r->spirv.iddecl == 0)
    {
        r->spirv.iddecl = spv_bumpid(ctx);
    } // if

    uint32 type;

    switch (regtype)
    {
        case REG_TYPE_ADDRESS:
        case REG_TYPE_PREDICATE:
        case REG_TYPE_LOOP:
        case REG_TYPE_LABEL:
            failf(ctx, "unimplemented regtype %d", regtype);
            return;
        case REG_TYPE_TEMP:
        {
            push_output(ctx, &ctx->mainline_intro);
            uint32 tid = spv_getptrvec4p(ctx);
            output_spvop(ctx, SpvOpVariable, 4);
            output_id(ctx, tid);
            output_id(ctx, r->spirv.iddecl);
            output_u32(ctx, SpvStorageClassPrivate);
            pop_output(ctx);

            char varname[64];
            get_SPIRV_varname_in_buf(ctx, r->regtype, r->regnum, varname, sizeof(varname));
            output_spvname(ctx, r->spirv.iddecl, varname);
            break;
        }

        default:
            assert(!"Unexpected regtype in emit_SPIRV_global");
    }
}

static void emit_SPIRV_array(Context *ctx, VariableList *var){}

static void emit_SPIRV_const_array(Context *ctx,
                                   const struct ConstantsList *constslist,
                                   int base, int size){}
static void emit_SPIRV_uniform(Context *ctx, RegisterType regtype, int regnum,
                               const VariableList *var)
{
    RegisterList *r = reglist_find(&ctx->uniforms, regtype, regnum);

    // TODO: If the SSA id for this register is still 0 by this point, that means no instructions actually
    // loaded from/stored to this variable...

    if (r->spirv.iddecl == 0)
    {
        r->spirv.iddecl = spv_bumpid(ctx);
    } // if

    if (var == NULL)
    {
        uint32 type;

        switch (regtype)
        {
            case REG_TYPE_CONST:
            case REG_TYPE_CONSTINT:
            case REG_TYPE_CONSTBOOL:
            {
                push_output(ctx, &ctx->mainline_intro);
                uint32 tid = spv_getptrvec4u(ctx);
                output_spvop(ctx, SpvOpVariable, 4);
                output_id(ctx, tid);
                output_id(ctx, r->spirv.iddecl);
                output_u32(ctx, SpvStorageClassUniform);
                pop_output(ctx);

                char varname[64];
                get_SPIRV_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));
                output_spvname(ctx, r->spirv.iddecl, varname);
                break;
            }

            default:
                fail(ctx, "BUG: used a uniform we don't know how to define.");
                break;
        }
    } // if

    else
    {
        const int arraybase = var->index;
        if (var->constant)
        {
            fail(ctx, "const array not implemented");
        } // if
        else
        {
            fail(ctx, "var->constant was NULL");
            // TODO: Double check after writing emit_SPIRV_array and emit_SPIRV_const_array whether to use emit_position
        } // else
    } // else
}
static void emit_SPIRV_sampler(Context *ctx, int stage, TextureType ttype,
                               int texbem){}

static void emit_SPIRV_attribute(Context *ctx, RegisterType regtype, int regnum,
                                 MOJOSHADER_usage usage, int index, int wmask,
                                 int flags)
{
    char varname[64];
    uint32 tid;
    RegisterList *r = spv_getreg(ctx, regtype, regnum);

    ctx->spirv.inoutcount += 1;

    // for OpName
    get_SPIRV_varname_in_buf(ctx, regtype, regnum, varname, sizeof (varname));

    if (shader_is_vertex(ctx))
    {
        // pre-vs3 output registers.
        // these don't ever happen in DCL opcodes, I think. Map to vs_3_*
        //  output registers.
        if (!shader_version_atleast(ctx, 3, 0))
        {
            if (regtype == REG_TYPE_RASTOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                index = regnum;
                switch ((const RastOutType) regnum)
                {
                    case RASTOUT_TYPE_POSITION:
                        usage = MOJOSHADER_USAGE_POSITION;
                        break;
                    case RASTOUT_TYPE_FOG:
                        usage = MOJOSHADER_USAGE_FOG;
                        break;
                    case RASTOUT_TYPE_POINT_SIZE:
                        usage = MOJOSHADER_USAGE_POINTSIZE;
                        break;
                } // switch
            } // if

            else if (regtype == REG_TYPE_ATTROUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_COLOR;
                index = regnum;
            } // else if

            else if (regtype == REG_TYPE_TEXCRDOUT)
            {
                regtype = REG_TYPE_OUTPUT;
                usage = MOJOSHADER_USAGE_TEXCOORD;
                index = regnum;
            } // else if
        } // if

        if (regtype == REG_TYPE_INPUT)
        {
            push_output(ctx, &ctx->mainline_intro);
            tid = spv_getptrvec4i(ctx);

            output_spvop(ctx, SpvOpVariable, 4);
            output_u32(ctx, tid);
            output_u32(ctx, r->spirv.iddecl);
            output_u32(ctx, SpvStorageClassInput);
            pop_output(ctx);

            output_spvname(ctx, r->spirv.iddecl, varname);
        } // if

        else if (regtype == REG_TYPE_OUTPUT)
        {
            push_output(ctx, &ctx->mainline_intro);
            tid = spv_getptrvec4o(ctx);

            output_spvop(ctx, SpvOpVariable, 4);
            output_u32(ctx, tid);
            output_u32(ctx, r->spirv.iddecl);
            output_u32(ctx, SpvStorageClassOutput);
            pop_output(ctx);

            output_spvname(ctx, r->spirv.iddecl, varname);
        } // else if

        else
        {
            fail(ctx, "unknown vertex shader attribute register");
        } // else
    } // if

    else if (shader_is_pixel(ctx))
    {
        // samplers DCLs get handled in emit_SPIRV_sampler().

        if (flags & MOD_CENTROID)  // !!! FIXME
        {
            failf(ctx, "centroid unsupported in %s profile", ctx->profile->name);
            return;
        } // if

        switch (regtype) {
            case REG_TYPE_COLOROUT:
                push_output(ctx, &ctx->mainline_intro);
                tid = spv_getptrvec4o(ctx);

                output_spvop(ctx, SpvOpVariable, 4);
                output_u32(ctx, tid);
                output_u32(ctx, r->spirv.iddecl);
                output_u32(ctx, SpvStorageClassOutput);
                pop_output(ctx);

                output_spvname(ctx, r->spirv.iddecl, varname);

                break;
            case REG_TYPE_DEPTHOUT:
                // maps to BuiltIn FragDepth
                push_output(ctx, &ctx->mainline_intro);
                tid = spv_getptrfloato(ctx);

                output_spvop(ctx, SpvOpVariable, 4);
                output_u32(ctx, tid);
                output_u32(ctx, r->spirv.iddecl);
                output_u32(ctx, SpvStorageClassOutput);
                pop_output(ctx);

                output_spvname(ctx, r->spirv.iddecl, varname);
                output_spvbuiltin(ctx, r->spirv.iddecl, SpvBuiltInFragDepth);

                break;
            case REG_TYPE_TEXTURE:
            case REG_TYPE_INPUT:
            case REG_TYPE_MISCTYPE:
                push_output(ctx, &ctx->mainline_intro);
                tid = spv_getptrvec4i(ctx);

                output_spvop(ctx, SpvOpVariable, 4);
                output_u32(ctx, tid);
                output_u32(ctx, r->spirv.iddecl);
                output_u32(ctx, SpvStorageClassInput);
                pop_output(ctx);

                output_spvname(ctx, r->spirv.iddecl, varname);

                break;
            default:
                fail(ctx, "unknown pixel shader attribute register");
        }
    } // else if

    else
    {
        fail(ctx, "Unknown shader type");  // state machine should catch this.
    } // else
} // emit_SPIRV_attribute

static void emit_SPIRV_finalize(Context *ctx)
{
    /* The generator's magic number, this could be registered with Khronos
     * if we wanted to. 0 is fine though, so use that for now. */
    uint32 genmagic = 0x00000000;

    /* Close main() */
    output_spvop(ctx, SpvOpReturn, 1);
    output_spvop(ctx, SpvOpFunctionEnd, 1);

    push_output(ctx, &ctx->preflight);

    output_u32(ctx, SpvMagicNumber);
    output_u32(ctx, SpvVersion);
    output_u32(ctx, genmagic);
    // "Bound: where all <id>s in this module are guaranteed to satisfy 0 < id < Bound"
    // `idmax` holds the last id that was given out, so we need to emit `idmax + 1`
    output_u32(ctx, ctx->spirv.idmax + 1);
    output_u32(ctx, 0);

    output_spvop(ctx, SpvOpCapability, 2);
    output_u32(ctx, SpvCapabilityShader);

    // only non-zero when actually needed
    if (ctx->spirv.idext)
    {
        const char *extstr = "GLSL.std.450";
        output_spvop(ctx, SpvOpExtInstImport, 2 + spv_strlen(extstr));
        output_u32(ctx, ctx->spirv.idext);
        output_spvstr(ctx, extstr);
    } // if

    output_spvop(ctx, SpvOpMemoryModel, 3);
    output_u32(ctx, SpvAddressingModelLogical);
    output_u32(ctx, SpvMemoryModelSimple);

    /* 3 is for opcode + exec. model + idmain */
    output_spvop(ctx, SpvOpEntryPoint, 3 + spv_strlen(ctx->mainfn) + ctx->spirv.inoutcount);
    if (shader_is_vertex(ctx))
    {
        output_u32(ctx, SpvExecutionModelVertex);
    } // if
    else if (shader_is_pixel(ctx))
    {
        output_u32(ctx, SpvExecutionModelFragment);
    } // else if
    output_u32(ctx, ctx->spirv.idmain);
    output_spvstr(ctx, ctx->mainfn);
    // attributes
    {
        char varname[64];
        RegisterList *p = &ctx->attributes, *r = NULL;

        // !!! FIXME: The first element of the list is always empty and I don't know why!
        p = p->next;
        while (p) {
            r = spv_getreg(ctx, p->regtype, p->regnum);
            get_SPIRV_varname_in_buf(ctx, p->regtype, p->regnum, varname, sizeof (varname));
            if (r) {
                output_id(ctx, r->spirv.iddecl);
            } else {
                failf(
                    ctx,
                    "missing attribute register %s (rt=%u, rn=%u, u=%u)",
                    varname, p->regtype, p->regnum, p->usage
                );
            }
            p = p->next;
        }
    }

    // only applies to pixel shaders
    if (shader_is_pixel(ctx))
    {
        output_spvop(ctx, SpvOpExecutionMode, 3);
        output_u32(ctx, ctx->spirv.idmain);
        output_u32(ctx, SpvExecutionModeOriginUpperLeft);
    } // if

    pop_output(ctx);

    componentlist_free(ctx, ctx->spirv.cl.f.next);
    componentlist_free(ctx, ctx->spirv.cl.i.next);
    componentlist_free(ctx, ctx->spirv.cl.u.next);
} // emit_SPIRV_finalize

static void emit_SPIRV_NOP(Context *ctx)
{
    // no-op is a no-op.  :)
} // emit_SPIRV_NOP

static void emit_SPIRV_DEF(Context *ctx)
{
    RegisterList *rl;
    uint32 val0, val1, val2, val3, idv4;
    const float *raw = (const float *) ctx->dwords;

    rl = spv_getreg(ctx, ctx->dest_arg.regtype, ctx->dest_arg.regnum);
    rl->spirv.iddecl = spv_bumpid(ctx);
    rl->spirv.iduse = rl->spirv.iddecl;

    val0 = spv_getscalarf(ctx, raw[0]);
    val1 = spv_getscalarf(ctx, raw[1]);
    val2 = spv_getscalarf(ctx, raw[2]);
    val3 = spv_getscalarf(ctx, raw[3]);

    idv4 = spv_getvec4(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpConstantComposite, 3 + 4);
    output_u32(ctx, idv4);
    output_u32(ctx, rl->spirv.iddecl);
    output_u32(ctx, val0);
    output_u32(ctx, val1);
    output_u32(ctx, val2);
    output_u32(ctx, val3);
    pop_output(ctx);
} // emit_SPIRV_DEF

static void emit_SPIRV_DEFI(Context *ctx)
{
    RegisterList *rl;
    uint32 val0, val1, val2, val3, idiv4;
    const int *raw = (const int *) ctx->dwords;

    rl = spv_getreg(ctx, ctx->dest_arg.regtype, ctx->dest_arg.regnum);
    rl->spirv.iddecl = spv_bumpid(ctx);
    rl->spirv.iduse = rl->spirv.iddecl;

    val0 = spv_getscalari(ctx, raw[0]);
    val1 = spv_getscalari(ctx, raw[1]);
    val2 = spv_getscalari(ctx, raw[2]);
    val3 = spv_getscalari(ctx, raw[3]);

    idiv4 = spv_getivec4(ctx);

    push_output(ctx, &ctx->mainline_intro);
    output_spvop(ctx, SpvOpConstantComposite, 3 + 4);
    output_u32(ctx, idiv4);
    output_u32(ctx, rl->spirv.iddecl);
    output_u32(ctx, val0);
    output_u32(ctx, val1);
    output_u32(ctx, val2);
    output_u32(ctx, val3);
    pop_output(ctx);
} // emit_SPIRV_DEFI

static void emit_SPIRV_DEFB(Context *ctx)
{
    RegisterList *rl = spv_getreg(ctx, ctx->dest_arg.regtype, ctx->dest_arg.regnum);
    rl->spirv.iddecl = ctx->dwords[0] ? spv_gettrue(ctx) : spv_getfalse(ctx);
    rl->spirv.iduse = rl->spirv.iddecl;
} // emit_SPIRV_DEFB

static void emit_SPIRV_DCL(Context *ctx)
{
    const RegisterType regtype = ctx->dest_arg.regtype;
    const int regnum = ctx->dest_arg.regnum;

    // state_DCL handles checking if the registers are valid for this instruction, and collecting samplers and attribs
    RegisterList *reg = spv_getreg(ctx, regtype, regnum);

    // This id will be assigned to in emit_SPIRV_attribute, but
    // emit_SPIRV_attribute is called after instructions are emitted,
    // so we generate the id here so it can be used in instructions
    reg->spirv.iddecl = spv_bumpid(ctx);
} // emit_SPIRV_DCL

static void _emit_SPIRV_dotproduct(Context *ctx, uint32 src0, uint32 src1)
{
    push_output(ctx, &ctx->mainline);

    uint32 float_tid = spv_getfloat(ctx);
    uint32 scalar_result = spv_bumpid(ctx);
    output_spvop(ctx, SpvOpDot, 5);
    output_id(ctx, float_tid);
    output_id(ctx, scalar_result);
    output_id(ctx, src0);
    output_id(ctx, src1);

    // Broadcast scalar result across all channels of a vec4i
    uint32 vector_result = spv_bumpid(ctx);
    uint32 vec4_tid = spv_getvec4(ctx);
    output_spvop(ctx, SpvOpCompositeConstruct, 3 + 4);
    output_id(ctx, vec4_tid);
    output_id(ctx, vector_result);
    for (int i = 0; i < 4; i++) output_id(ctx, scalar_result);

    pop_output(ctx);

    spv_assign_destarg(ctx, vector_result);
}

static void emit_SPIRV_DP4(Context *ctx)
{
    uint32 src0 = spv_load_srcarg_full(ctx, 0);
    uint32 src1 = spv_load_srcarg_full(ctx, 1);

    _emit_SPIRV_dotproduct(ctx, src0, src1);
}

static void emit_SPIRV_DP3(Context *ctx)
{
    uint32 src0 = spv_load_srcarg_vec3(ctx, 0);
    uint32 src1 = spv_load_srcarg_vec3(ctx, 1);

    _emit_SPIRV_dotproduct(ctx, src0, src1);
}

#define MAKE_SPIRV_EMITTER_DSS(name, emit_spvop) \
    static void emit_SPIRV_ ## name(Context *ctx) \
    { \
        uint32 src0 = spv_load_srcarg_full(ctx, 0); \
        uint32 src1 = spv_load_srcarg_full(ctx, 1); \
        uint32 result = spv_bumpid(ctx); \
        uint32 rtid = spv_getvec4(ctx); \
        \
        push_output(ctx, &ctx->mainline); \
        emit_spvop; \
        pop_output(ctx); \
        \
        spv_assign_destarg(ctx, result); \
    }

MAKE_SPIRV_EMITTER_DSS(ADD, {
    output_spvop(ctx, SpvOpFAdd, 5);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

MAKE_SPIRV_EMITTER_DSS(SUB, {
    output_spvop(ctx, SpvOpFSub, 5);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

MAKE_SPIRV_EMITTER_DSS(MUL, {
    output_spvop(ctx, SpvOpFMul, 5);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

MAKE_SPIRV_EMITTER_DSS(SLT, {
    // https://msdn.microsoft.com/en-us/library/windows/desktop/cc308050(v=vs.85).aspx
    // "The comparisons EQ, GT, GE, LT, and LE, when either or both operands is NaN returns FALSE"
    output_spvop(ctx, SpvOpFOrdLessThan, 5);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

MAKE_SPIRV_EMITTER_DSS(SGE, {
    // https://msdn.microsoft.com/en-us/library/windows/desktop/cc308050(v=vs.85).aspx
    // "The comparisons EQ, GT, GE, LT, and LE, when either or both operands is NaN returns FALSE"
    output_spvop(ctx, SpvOpFOrdGreaterThanEqual, 5);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

MAKE_SPIRV_EMITTER_DSS(MIN, {
    output_spvop(ctx, SpvOpExtInst, 5 + 2);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450FMin);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

MAKE_SPIRV_EMITTER_DSS(MAX, {
    output_spvop(ctx, SpvOpExtInst, 5 + 2);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450FMax);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

MAKE_SPIRV_EMITTER_DSS(POW, {
    output_spvop(ctx, SpvOpExtInst, 5 + 2);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450Pow);
    output_id(ctx, src0);
    output_id(ctx, src1);
})

static uint32 SPIRV__extract_vec3(Context *ctx, uint32 input) {
    uint32 vec3 = spv_getvec3(ctx);
    uint32 result = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline);

    output_spvop(ctx, SpvOpVectorShuffle, 5 + 3);
    output_id(ctx, vec3);
    output_id(ctx, result);
    output_id(ctx, input);
    output_id(ctx, input);
    output_u32(ctx, 0);
    output_u32(ctx, 1);
    output_u32(ctx, 2);

    pop_output(ctx);

    return result;
}

MAKE_SPIRV_EMITTER_DSS(CRS, {
    uint32 vec3 = spv_getvec3(ctx);
    uint32 src0_vec3 = SPIRV__extract_vec3(ctx, src0);
    uint32 src1_vec3 = SPIRV__extract_vec3(ctx, src1);
    uint32 result_vec3 = spv_bumpid(ctx);

    output_spvop(ctx, SpvOpExtInst, 5 + 2);
    output_id(ctx, vec3);
    output_id(ctx, result_vec3);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450Cross);
    output_id(ctx, src0_vec3);
    output_id(ctx, src1_vec3);

    output_spvop(ctx, SpvOpVectorShuffle, 5 + 4);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, result_vec3);
    output_id(ctx, result_vec3);
    output_u32(ctx, 0);
    output_u32(ctx, 1);
    output_u32(ctx, 2);
    // According to DirectX docs, CRS doesn't allow `w` in its writemask, so we can make this component anything and the
    // code generated by `spv_assign_destarg()` will just throw it away.
    output_u32(ctx, 0xFFFFFFFF);
})

static void emit_SPIRV_MAD (Context *ctx)
{
    uint32 src0 = spv_load_srcarg_full(ctx, 0);
    uint32 src1 = spv_load_srcarg_full(ctx, 1);
    uint32 src2 = spv_load_srcarg_full(ctx, 2);
    uint32 mul_result = spv_bumpid(ctx);
    uint32 result = spv_bumpid(ctx);
    uint32 vec4 = spv_getvec4(ctx);

    push_output(ctx, &ctx->mainline);

    output_spvop(ctx, SpvOpFMul, 5);
    output_id(ctx, vec4);
    output_id(ctx, mul_result);
    output_id(ctx, src0);
    output_id(ctx, src1);

    output_spvop(ctx, SpvOpFAdd, 5);
    output_id(ctx, vec4);
    output_id(ctx, result);
    output_id(ctx, mul_result);
    output_id(ctx, src2);

    pop_output(ctx);

    spv_assign_destarg(ctx, result);
}

#define MAKE_SPIRV_EMITTER_DS(name, emit_spvop) \
    static void emit_SPIRV_ ## name(Context *ctx) \
    { \
        uint32 src0 = spv_load_srcarg_full(ctx, 0); \
        uint32 result = spv_bumpid(ctx); \
        uint32 rtid = spv_getvec4(ctx); \
        \
        push_output(ctx, &ctx->mainline); \
        emit_spvop; \
        pop_output(ctx); \
        \
        spv_assign_destarg(ctx, result); \
    }

MAKE_SPIRV_EMITTER_DS(MOV, {
    // This wastes the SSA id allocated for result, but do I really want to ruin a function this simple
    result = src0;
})

MAKE_SPIRV_EMITTER_DS(RCP, {
    uint32 one = spv_getscalarf(ctx, 1.0f);

    output_spvop(ctx, SpvOpFDiv, 5);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, one);
    output_id(ctx, src0);
})

MAKE_SPIRV_EMITTER_DS(RSQ, {
    output_spvop(ctx, SpvOpExtInst, 5 + 1);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450InverseSqrt);
    output_id(ctx, src0);
})

MAKE_SPIRV_EMITTER_DS(EXP, {
    output_spvop(ctx, SpvOpExtInst, 5 + 1);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450Exp);
    output_id(ctx, src0);
})

MAKE_SPIRV_EMITTER_DS(SGN, {
    // SGN also takes a src1 and src2 to use for intermediate results, they are left undefined after the instruction
    // executes, and as such it is perfectly valid for us to not touch those registers in our implementation
    output_spvop(ctx, SpvOpExtInst, 5 + 1);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450FSign);
    output_id(ctx, src0);
})

MAKE_SPIRV_EMITTER_DS(ABS, {
    output_spvop(ctx, SpvOpExtInst, 5 + 1);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450FAbs);
    output_id(ctx, src0);
})

MAKE_SPIRV_EMITTER_DS(NRM, {
    output_spvop(ctx, SpvOpExtInst, 5 + 1);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450Normalize);
    output_id(ctx, src0);
})

MAKE_SPIRV_EMITTER_DS(FRC, {
    output_spvop(ctx, SpvOpExtInst, 5 + 1);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450Fract);
    output_id(ctx, src0);
})

static void emit_SPIRV_LRP(Context *ctx)
{
    // lerp(x, y, a) = x + a*(y - x)
    //               = x*(1 - a) + y*a
    uint32 a = spv_load_srcarg_full(ctx, 0); // 'scale'
    uint32 y = spv_load_srcarg_full(ctx, 1); // 'end'
    uint32 x = spv_load_srcarg_full(ctx, 2); // 'start'
    uint32 result = spv_bumpid(ctx);
    uint32 rtid = spv_getvec4(ctx);

    push_output(ctx, &ctx->mainline);

    output_spvop(ctx, SpvOpExtInst, 5 + 3);
    output_id(ctx, rtid);
    output_id(ctx, result);
    output_id(ctx, spv_getext(ctx));
    output_id(ctx, GLSLstd450FMix);
    output_id(ctx, x);
    output_id(ctx, y);
    output_id(ctx, a);

    pop_output(ctx);

    spv_assign_destarg(ctx, result);
}

const int NULL_SWIZZLE = 0xE4; // 0xE4 == 11100100 ... 0 1 2 3. No swizzle.

static void _emit_SPIRV_vecXmatrix(Context *ctx, int rows, int writemask)
{
    assert(rows <= 4);
    assert(writemask == 0x7 || writemask == 0xF);

    uint32 src0 = spv_load_srcarg(ctx, 0, writemask);
    uint32 float_tid = spv_getfloat(ctx);
    uint32 vec4_tid = spv_getvec4(ctx);

    RegisterType src1type = ctx->source_args[1].regtype;
    int src1num = ctx->source_args[1].regnum;


    uint32 result_components[4];
    for (int i = 0; i < rows; i++)
    {
        uint32 row = spv_loadreg(ctx, spv_getreg(ctx, src1type, src1num + i));
        row = spv_swizzle(ctx, row, NULL_SWIZZLE, writemask);
        uint32 dot_result = spv_bumpid(ctx);

        push_output(ctx, &ctx->mainline);
        output_spvop(ctx, SpvOpDot, 5);
        output_id(ctx, float_tid);
        output_id(ctx, dot_result);
        output_id(ctx, src0);
        output_id(ctx, row);
        pop_output(ctx);

        result_components[i] = dot_result;
    }

    uint32 result = spv_bumpid(ctx);

    push_output(ctx, &ctx->mainline);
    output_spvop(ctx, SpvOpCompositeConstruct, 3 + rows);
    output_id(ctx, vec4_tid);
    output_id(ctx, result);
    for (int i = 0; i < rows; i++) output_id(ctx, result_components[i]);
    pop_output(ctx);

    spv_assign_destarg(ctx, result);
}

static void emit_SPIRV_M4X4(Context *ctx)
{
    // float4 * (4 columns, 4 rows) -> float4
    _emit_SPIRV_vecXmatrix(ctx, 4, 0xF);
}

static void emit_SPIRV_M4X3(Context *ctx)
{
    // float4 * (4 columns, 3 rows) -> float3
    _emit_SPIRV_vecXmatrix(ctx, 3, 0xF);
}

static void emit_SPIRV_M3X4(Context *ctx)
{
    // float3 * (3 columns, 4 rows) -> float4
    _emit_SPIRV_vecXmatrix(ctx, 4, 0x7);
}

static void emit_SPIRV_M3X3(Context *ctx)
{
    // float3 * (3 columns, 3 rows) -> float3
    _emit_SPIRV_vecXmatrix(ctx, 3, 0x7);
}

static void emit_SPIRV_M3X2(Context *ctx)
{
    // float3 * (3 columns, 2 rows) -> float2
    _emit_SPIRV_vecXmatrix(ctx, 2, 0x7);
}

//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(MOV)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(ADD)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(SUB)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(MAD)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(MUL)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(RCP)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(RSQ)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DP3)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DP4)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(MIN)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(MAX)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(SLT)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(SGE)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(EXP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(LOG)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(LIT)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DST)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(LRP)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(FRC)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(M4X4)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(M4X3)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(M3X4)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(M3X3)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(M3X2)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(CALL)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(CALLNZ)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(LOOP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(RET)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(ENDLOOP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(LABEL)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DCL)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(POW)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(CRS)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(SGN)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(ABS)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(NRM)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(SINCOS)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(REP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(ENDREP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(IF)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(IFC)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(ELSE)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(ENDIF)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(BREAK)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(BREAKC)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(MOVA)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DEFB)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DEFI)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(RESERVED)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXCRD)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXKILL)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXLD)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXBEM)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXBEML)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2AR)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2GB)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X2PAD)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X2TEX)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X3PAD)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X3TEX)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X3SPEC)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X3VSPEC)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(EXPP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(LOGP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(CND)
//EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DEF)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXREG2RGB)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3TEX)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X2DEPTH)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXDP3)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXM3X3)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXDEPTH)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(CMP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(BEM)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DP2ADD)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DSX)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(DSY)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXLDD)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(SETP)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(TEXLDL)
EMIT_SPIRV_OPCODE_UNIMPLEMENTED_FUNC(BREAKP)

#endif  // SUPPORT_PROFILE_SPIRV

#if !AT_LEAST_ONE_PROFILE
#error No profiles are supported. Fix your build.
#endif

#define DEFINE_PROFILE(prof) { \
    MOJOSHADER_PROFILE_##prof, \
    emit_##prof##_start, \
    emit_##prof##_end, \
    emit_##prof##_phase, \
    emit_##prof##_global, \
    emit_##prof##_array, \
    emit_##prof##_const_array, \
    emit_##prof##_uniform, \
    emit_##prof##_sampler, \
    emit_##prof##_attribute, \
    emit_##prof##_finalize, \
    get_##prof##_varname, \
    get_##prof##_const_array_varname, \
},

static const Profile profiles[] =
{
#if SUPPORT_PROFILE_D3D
    DEFINE_PROFILE(D3D)
#endif
#if SUPPORT_PROFILE_BYTECODE
    DEFINE_PROFILE(BYTECODE)
#endif
#if SUPPORT_PROFILE_GLSL
    DEFINE_PROFILE(GLSL)
#endif
#if SUPPORT_PROFILE_ARB1
    DEFINE_PROFILE(ARB1)
#endif
#if SUPPORT_PROFILE_METAL
    DEFINE_PROFILE(METAL)
#endif
#if SUPPORT_PROFILE_SPIRV
    DEFINE_PROFILE(SPIRV)
#endif
};

#undef DEFINE_PROFILE

// This is for profiles that extend other profiles...
static const struct { const char *from; const char *to; } profileMap[] =
{
    { MOJOSHADER_PROFILE_GLSLES, MOJOSHADER_PROFILE_GLSL },
    { MOJOSHADER_PROFILE_GLSL120, MOJOSHADER_PROFILE_GLSL },
    { MOJOSHADER_PROFILE_NV2, MOJOSHADER_PROFILE_ARB1 },
    { MOJOSHADER_PROFILE_NV3, MOJOSHADER_PROFILE_ARB1 },
    { MOJOSHADER_PROFILE_NV4, MOJOSHADER_PROFILE_ARB1 },
};


// The PROFILE_EMITTER_* items MUST be in the same order as profiles[]!
#define PROFILE_EMITTERS(op) { \
     PROFILE_EMITTER_D3D(op) \
     PROFILE_EMITTER_BYTECODE(op) \
     PROFILE_EMITTER_GLSL(op) \
     PROFILE_EMITTER_ARB1(op) \
     PROFILE_EMITTER_METAL(op) \
     PROFILE_EMITTER_SPIRV(op) \
}

static int parse_destination_token(Context *ctx, DestArgInfo *info)
{
    // !!! FIXME: recheck against the spec for ranges (like RASTOUT values, etc).
    if (ctx->tokencount == 0)
    {
        fail(ctx, "Out of tokens in destination parameter");
        return 0;
    } // if

    const uint32 token = SWAP32(*(ctx->tokens));
    const int reserved1 = (int) ((token >> 14) & 0x3); // bits 14 through 15
    const int reserved2 = (int) ((token >> 31) & 0x1); // bit 31

    info->token = ctx->tokens;
    info->regnum = (int) (token & 0x7ff);  // bits 0 through 10
    info->relative = (int) ((token >> 13) & 0x1); // bit 13
    info->orig_writemask = (int) ((token >> 16) & 0xF); // bits 16 through 19
    info->result_mod = (int) ((token >> 20) & 0xF); // bits 20 through 23
    info->result_shift = (int) ((token >> 24) & 0xF); // bits 24 through 27      abc
    info->regtype = (RegisterType) (((token >> 28) & 0x7) | ((token >> 8) & 0x18));  // bits 28-30, 11-12

    int writemask;
    if (isscalar(ctx, ctx->shader_type, info->regtype, info->regnum))
        writemask = 0x1;  // just x.
    else
        writemask = info->orig_writemask;

    set_dstarg_writemask(info, writemask);  // bits 16 through 19.

    // all the REG_TYPE_CONSTx types are the same register type, it's just
    //  split up so its regnum can be > 2047 in the bytecode. Clean it up.
    if (info->regtype == REG_TYPE_CONST2)
    {
        info->regtype = REG_TYPE_CONST;
        info->regnum += 2048;
    } // else if
    else if (info->regtype == REG_TYPE_CONST3)
    {
        info->regtype = REG_TYPE_CONST;
        info->regnum += 4096;
    } // else if
    else if (info->regtype == REG_TYPE_CONST4)
    {
        info->regtype = REG_TYPE_CONST;
        info->regnum += 6144;
    } // else if

    // swallow token for now, for multiple calls in a row.
    adjust_token_position(ctx, 1);

    if (reserved1 != 0x0)
        fail(ctx, "Reserved bit #1 in destination token must be zero");

    if (reserved2 != 0x1)
        fail(ctx, "Reserved bit #2 in destination token must be one");

    if (info->relative)
    {
        if (!shader_is_vertex(ctx))
            fail(ctx, "Relative addressing in non-vertex shader");
        if (!shader_version_atleast(ctx, 3, 0))
            fail(ctx, "Relative addressing in vertex shader version < 3.0");
        if ((!ctx->ctab.have_ctab) && (!ctx->ignores_ctab))
        {
            // it's hard to do this efficiently without!
            fail(ctx, "relative addressing unsupported without a CTAB");
        } // if

        // !!! FIXME: I don't have a shader that has a relative dest currently.
        fail(ctx, "Relative addressing of dest tokens is unsupported");
        return 2;
    } // if

    const int s = info->result_shift;
    if (s != 0)
    {
        if (!shader_is_pixel(ctx))
            fail(ctx, "Result shift scale in non-pixel shader");
        if (shader_version_atleast(ctx, 2, 0))
            fail(ctx, "Result shift scale in pixel shader version >= 2.0");
        if ( ! (((s >= 1) && (s <= 3)) || ((s >= 0xD) && (s <= 0xF))) )
            fail(ctx, "Result shift scale isn't 1 to 3, or 13 to 15.");
    } // if

    if (info->result_mod & MOD_PP)  // Partial precision (pixel shaders only)
    {
        if (!shader_is_pixel(ctx))
            fail(ctx, "Partial precision result mod in non-pixel shader");
    } // if

    if (info->result_mod & MOD_CENTROID)  // Centroid (pixel shaders only)
    {
        if (!shader_is_pixel(ctx))
            fail(ctx, "Centroid result mod in non-pixel shader");
        else if (!ctx->centroid_allowed)  // only on DCL opcodes!
            fail(ctx, "Centroid modifier not allowed here");
    } // if

    if (/*(info->regtype < 0) ||*/ (info->regtype > REG_TYPE_MAX))
        fail(ctx, "Register type is out of range");

    if (!isfail(ctx))
        set_used_register(ctx, info->regtype, info->regnum, 1);

    return 1;
} // parse_destination_token


static void determine_constants_arrays(Context *ctx)
{
    // Only process this stuff once. This is called after all DEF* opcodes
    //  could have been parsed.
    if (ctx->determined_constants_arrays)
        return;

    ctx->determined_constants_arrays = 1;

    if (ctx->constant_count <= 1)
        return;  // nothing to sort or group.

    // Sort the linked list into an array for easier tapdancing...
    ConstantsList **array = (ConstantsList **) alloca(sizeof (ConstantsList *) * (ctx->constant_count + 1));
    ConstantsList *item = ctx->constants;
    int i;

    for (i = 0; i < ctx->constant_count; i++)
    {
        if (item == NULL)
        {
            fail(ctx, "BUG: mismatched constant list and count");
            return;
        } // if

        array[i] = item;
        item = item->next;
    } // for

    array[ctx->constant_count] = NULL;

    // bubble sort ftw.
    int sorted;
    do
    {
        sorted = 1;
        for (i = 0; i < ctx->constant_count-1; i++)
        {
            if (array[i]->constant.index > array[i+1]->constant.index)
            {
                ConstantsList *tmp = array[i];
                array[i] = array[i+1];
                array[i+1] = tmp;
                sorted = 0;
            } // if
        } // for
    } while (!sorted);

    // okay, sorted. While we're here, let's redo the linked list in order...
    for (i = 0; i < ctx->constant_count; i++)
        array[i]->next = array[i+1];
    ctx->constants = array[0];

    // now figure out the groupings of constants and add to ctx->variables...
    int start = -1;
    int prev = -1;
    int count = 0;
    const int hi = ctx->constant_count;
    for (i = 0; i <= hi; i++)
    {
        if (array[i] && (array[i]->constant.type != MOJOSHADER_UNIFORM_FLOAT))
            continue;  // we only care about REG_TYPE_CONST for array groups.

        if (start == -1)
        {
            prev = start = i;  // first REG_TYPE_CONST we've seen. Mark it!
            continue;
        } // if

        // not a match (or last item in the array)...see if we had a
        //  contiguous set before this point...
        if ( (array[i]) && (array[i]->constant.index == (array[prev]->constant.index + 1)) )
            count++;
        else
        {
            if (count > 0)  // multiple constants in the set?
            {
                VariableList *var;
                var = (VariableList *) Malloc(ctx, sizeof (VariableList));
                if (var == NULL)
                    break;

                var->type = MOJOSHADER_UNIFORM_FLOAT;
                var->index = array[start]->constant.index;
                var->count = (array[prev]->constant.index - var->index) + 1;
                var->constant = array[start];
                var->used = 0;
                var->emit_position = -1;
                var->next = ctx->variables;
                ctx->variables = var;
            } // if

            start = i;   // set this as new start of sequence.
        } // if

        prev = i;
    } // for
} // determine_constants_arrays


static int adjust_swizzle(const Context *ctx, const RegisterType regtype,
                          const int regnum, const int swizzle)
{
    if (regtype != REG_TYPE_INPUT)  // !!! FIXME: maybe lift this later?
        return swizzle;
    else if (ctx->swizzles_count == 0)
        return swizzle;

    const RegisterList *reg = reglist_find(&ctx->attributes, regtype, regnum);
    if (reg == NULL)
        return swizzle;

    size_t i;
    for (i = 0; i < ctx->swizzles_count; i++)
    {
        const MOJOSHADER_swizzle *swiz = &ctx->swizzles[i];
        if ((swiz->usage == reg->usage) && (swiz->index == reg->index))
        {
            return ( (((int)(swiz->swizzles[((swizzle >> 0) & 0x3)])) << 0) |
                     (((int)(swiz->swizzles[((swizzle >> 2) & 0x3)])) << 2) |
                     (((int)(swiz->swizzles[((swizzle >> 4) & 0x3)])) << 4) |
                     (((int)(swiz->swizzles[((swizzle >> 6) & 0x3)])) << 6) );
        } // if
    } // for

    return swizzle;
} // adjust_swizzle


static int parse_source_token(Context *ctx, SourceArgInfo *info)
{
    int retval = 1;

    if (ctx->tokencount == 0)
    {
        fail(ctx, "Out of tokens in source parameter");
        return 0;
    } // if

    const uint32 token = SWAP32(*(ctx->tokens));
    const int reserved1 = (int) ((token >> 14) & 0x3); // bits 14 through 15
    const int reserved2 = (int) ((token >> 31) & 0x1); // bit 31

    info->token = ctx->tokens;
    info->regnum = (int) (token & 0x7ff);  // bits 0 through 10
    info->relative = (int) ((token >> 13) & 0x1); // bit 13
    const int swizzle = (int) ((token >> 16) & 0xFF); // bits 16 through 23
    info->src_mod = (SourceMod) ((token >> 24) & 0xF); // bits 24 through 27
    info->regtype = (RegisterType) (((token >> 28) & 0x7) | ((token >> 8) & 0x18));  // bits 28-30, 11-12

    // all the REG_TYPE_CONSTx types are the same register type, it's just
    //  split up so its regnum can be > 2047 in the bytecode. Clean it up.
    if (info->regtype == REG_TYPE_CONST2)
    {
        info->regtype = REG_TYPE_CONST;
        info->regnum += 2048;
    } // else if
    else if (info->regtype == REG_TYPE_CONST3)
    {
        info->regtype = REG_TYPE_CONST;
        info->regnum += 4096;
    } // else if
    else if (info->regtype == REG_TYPE_CONST4)
    {
        info->regtype = REG_TYPE_CONST;
        info->regnum += 6144;
    } // else if

    info->swizzle = adjust_swizzle(ctx, info->regtype, info->regnum, swizzle);
    info->swizzle_x = ((info->swizzle >> 0) & 0x3);
    info->swizzle_y = ((info->swizzle >> 2) & 0x3);
    info->swizzle_z = ((info->swizzle >> 4) & 0x3);
    info->swizzle_w = ((info->swizzle >> 6) & 0x3);

    // swallow token for now, for multiple calls in a row.
    adjust_token_position(ctx, 1);

    if (reserved1 != 0x0)
        fail(ctx, "Reserved bits #1 in source token must be zero");

    if (reserved2 != 0x1)
        fail(ctx, "Reserved bit #2 in source token must be one");

    if ((info->relative) && (ctx->tokencount == 0))
    {
        fail(ctx, "Out of tokens in relative source parameter");
        info->relative = 0;  // don't try to process it.
    } // if

    if (info->relative)
    {
        if ( (shader_is_pixel(ctx)) && (!shader_version_atleast(ctx, 3, 0)) )
            fail(ctx, "Relative addressing in pixel shader version < 3.0");

        // Shader Model 1 doesn't have an extra token to specify the
        //  relative register: it's always a0.x.
        if (!shader_version_atleast(ctx, 2, 0))
        {
            info->relative_regnum = 0;
            info->relative_regtype = REG_TYPE_ADDRESS;
            info->relative_component = 0;
        } // if

        else  // Shader Model 2 and later...
        {
            const uint32 reltoken = SWAP32(*(ctx->tokens));
            // swallow token for now, for multiple calls in a row.
            adjust_token_position(ctx, 1);

            const int relswiz = (int) ((reltoken >> 16) & 0xFF);
            info->relative_regnum = (int) (reltoken & 0x7ff);
            info->relative_regtype = (RegisterType)
                                        (((reltoken >> 28) & 0x7) |
                                        ((reltoken >> 8) & 0x18));

            if (((reltoken >> 31) & 0x1) == 0)
                fail(ctx, "bit #31 in relative address must be set");

            if ((reltoken & 0xF00E000) != 0)  // usused bits.
                fail(ctx, "relative address reserved bit must be zero");

            switch (info->relative_regtype)
            {
                case REG_TYPE_LOOP:
                case REG_TYPE_ADDRESS:
                    break;
                default:
                    fail(ctx, "invalid register for relative address");
                    break;
            } // switch

            if (info->relative_regnum != 0)  // true for now.
                fail(ctx, "invalid register for relative address");

            if (!replicate_swizzle(relswiz))
                fail(ctx, "relative address needs replicate swizzle");

            info->relative_component = (relswiz & 0x3);

            retval++;
        } // else

        if (info->regtype == REG_TYPE_INPUT)
        {
            if ( (shader_is_pixel(ctx)) || (!shader_version_atleast(ctx, 3, 0)) )
                fail(ctx, "relative addressing of input registers not supported in this shader model");
            ctx->have_relative_input_registers = 1;
        } // if
        else if (info->regtype == REG_TYPE_CONST)
        {
            // figure out what array we're in...
            if (!ctx->ignores_ctab)
            {
                if (!ctx->ctab.have_ctab)  // hard to do efficiently without!
                    fail(ctx, "relative addressing unsupported without a CTAB");
                else
                {
                    determine_constants_arrays(ctx);

                    VariableList *var;
                    const int reltarget = info->regnum;
                    for (var = ctx->variables; var != NULL; var = var->next)
                    {
                        const int lo = var->index;
                        if ( (reltarget >= lo) && (reltarget < (lo + var->count)) )
                            break;  // match!
                    } // for

                    if (var == NULL)
                        fail(ctx, "relative addressing of indeterminate array");
                    else
                    {
                        var->used = 1;
                        info->relative_array = var;
                        set_used_register(ctx, info->relative_regtype, info->relative_regnum, 0);
                    } // else
                } // else
            } // if
        } // else if
        else
        {
            fail(ctx, "relative addressing of invalid register");
        } // else
    } // if

    switch (info->src_mod)
    {
        case SRCMOD_NONE:
        case SRCMOD_ABSNEGATE:
        case SRCMOD_ABS:
        case SRCMOD_NEGATE:
            break; // okay in any shader model.

        // apparently these are only legal in Shader Model 1.x ...
        case SRCMOD_BIASNEGATE:
        case SRCMOD_BIAS:
        case SRCMOD_SIGNNEGATE:
        case SRCMOD_SIGN:
        case SRCMOD_COMPLEMENT:
        case SRCMOD_X2NEGATE:
        case SRCMOD_X2:
        case SRCMOD_DZ:
        case SRCMOD_DW:
            if (shader_version_atleast(ctx, 2, 0))
                fail(ctx, "illegal source mod for this Shader Model.");
            break;

        case SRCMOD_NOT:  // !!! FIXME: I _think_ this is right...
            if (shader_version_atleast(ctx, 2, 0))
            {
                if (info->regtype != REG_TYPE_PREDICATE
                 && info->regtype != REG_TYPE_CONSTBOOL)
                    fail(ctx, "NOT only allowed on bool registers.");
            } // if
            break;

        default:
            fail(ctx, "Unknown source modifier");
    } // switch

    // !!! FIXME: docs say this for sm3 ... check these!
    //  "The negate modifier cannot be used on second source register of these
    //   instructions: m3x2 - ps, m3x3 - ps, m3x4 - ps, m4x3 - ps, and
    //   m4x4 - ps."
    //  "If any version 3 shader reads from one or more constant float
    //   registers (c#), one of the following must be true.
    //    All of the constant floating-point registers must use the abs modifier.
    //    None of the constant floating-point registers can use the abs modifier.

    if (!isfail(ctx))
    {
        RegisterList *reg;
        reg = set_used_register(ctx, info->regtype, info->regnum, 0);
        // !!! FIXME: this test passes if you write to the register
        // !!! FIXME:  in this same instruction, because we parse the
        // !!! FIXME:  destination token first.
        // !!! FIXME: Microsoft's shader validation explicitly checks temp
        // !!! FIXME:  registers for this...do they check other writable ones?
        if ((info->regtype == REG_TYPE_TEMP) && (reg) && (!reg->written))
            failf(ctx, "Temp register r%d used uninitialized", info->regnum);
    } // if

    return retval;
} // parse_source_token


static int parse_predicated_token(Context *ctx)
{
    SourceArgInfo *arg = &ctx->predicate_arg;
    parse_source_token(ctx, arg);
    if (arg->regtype != REG_TYPE_PREDICATE)
        fail(ctx, "Predicated instruction but not predicate register!");
    if ((arg->src_mod != SRCMOD_NONE) && (arg->src_mod != SRCMOD_NOT))
        fail(ctx, "Predicated instruction register is not NONE or NOT");
    if ( !no_swizzle(arg->swizzle) && !replicate_swizzle(arg->swizzle) )
        fail(ctx, "Predicated instruction register has wrong swizzle");
    if (arg->relative)  // I'm pretty sure this is illegal...?
        fail(ctx, "relative addressing in predicated token");

    return 1;
} // parse_predicated_token


static int parse_args_NULL(Context *ctx)
{
    return 1;
} // parse_args_NULL


static int parse_args_DEF(Context *ctx)
{
    parse_destination_token(ctx, &ctx->dest_arg);
    if (ctx->dest_arg.regtype != REG_TYPE_CONST)
        fail(ctx, "DEF using non-CONST register");
    if (ctx->dest_arg.relative)  // I'm pretty sure this is illegal...?
        fail(ctx, "relative addressing in DEF");

    ctx->dwords[0] = SWAP32(ctx->tokens[0]);
    ctx->dwords[1] = SWAP32(ctx->tokens[1]);
    ctx->dwords[2] = SWAP32(ctx->tokens[2]);
    ctx->dwords[3] = SWAP32(ctx->tokens[3]);

    return 6;
} // parse_args_DEF


static int parse_args_DEFI(Context *ctx)
{
    parse_destination_token(ctx, &ctx->dest_arg);
    if (ctx->dest_arg.regtype != REG_TYPE_CONSTINT)
        fail(ctx, "DEFI using non-CONSTING register");
    if (ctx->dest_arg.relative)  // I'm pretty sure this is illegal...?
        fail(ctx, "relative addressing in DEFI");

    ctx->dwords[0] = SWAP32(ctx->tokens[0]);
    ctx->dwords[1] = SWAP32(ctx->tokens[1]);
    ctx->dwords[2] = SWAP32(ctx->tokens[2]);
    ctx->dwords[3] = SWAP32(ctx->tokens[3]);

    return 6;
} // parse_args_DEFI


static int parse_args_DEFB(Context *ctx)
{
    parse_destination_token(ctx, &ctx->dest_arg);
    if (ctx->dest_arg.regtype != REG_TYPE_CONSTBOOL)
        fail(ctx, "DEFB using non-CONSTBOOL register");
    if (ctx->dest_arg.relative)  // I'm pretty sure this is illegal...?
        fail(ctx, "relative addressing in DEFB");

    ctx->dwords[0] = *(ctx->tokens) ? 1 : 0;

    return 3;
} // parse_args_DEFB


static int valid_texture_type(const uint32 ttype)
{
    switch ((const TextureType) ttype)
    {
        case TEXTURE_TYPE_2D:
        case TEXTURE_TYPE_CUBE:
        case TEXTURE_TYPE_VOLUME:
            return 1;  // it's okay.
    } // switch

    return 0;
} // valid_texture_type


// !!! FIXME: this function is kind of a mess.
static int parse_args_DCL(Context *ctx)
{
    int unsupported = 0;
    const uint32 token = SWAP32(*(ctx->tokens));
    const int reserved1 = (int) ((token >> 31) & 0x1); // bit 31
    uint32 reserved_mask = 0x00000000;

    if (reserved1 != 0x1)
        fail(ctx, "Bit #31 in DCL token must be one");

    ctx->centroid_allowed = 1;
    adjust_token_position(ctx, 1);
    parse_destination_token(ctx, &ctx->dest_arg);
    ctx->centroid_allowed = 0;

    if (ctx->dest_arg.result_shift != 0)  // I'm pretty sure this is illegal...?
        fail(ctx, "shift scale in DCL");
    if (ctx->dest_arg.relative)  // I'm pretty sure this is illegal...?
        fail(ctx, "relative addressing in DCL");

    const RegisterType regtype = ctx->dest_arg.regtype;
    const int regnum = ctx->dest_arg.regnum;
    if ( (shader_is_pixel(ctx)) && (shader_version_atleast(ctx, 3, 0)) )
    {
        if (regtype == REG_TYPE_INPUT)
        {
            const uint32 usage = (token & 0xF);
            const uint32 index = ((token >> 16) & 0xF);
            reserved_mask = 0x7FF0FFE0;
            ctx->dwords[0] = usage;
            ctx->dwords[1] = index;
        } // if

        else if (regtype == REG_TYPE_MISCTYPE)
        {
            const MiscTypeType mt = (MiscTypeType) regnum;
            if (mt == MISCTYPE_TYPE_POSITION)
                reserved_mask = 0x7FFFFFFF;
            else if (mt == MISCTYPE_TYPE_FACE)
            {
                reserved_mask = 0x7FFFFFFF;
                if (!writemask_xyzw(ctx->dest_arg.orig_writemask))
                    fail(ctx, "DCL face writemask must be full");
                if (ctx->dest_arg.result_mod != 0)
                    fail(ctx, "DCL face result modifier must be zero");
                if (ctx->dest_arg.result_shift != 0)
                    fail(ctx, "DCL face shift scale must be zero");
            } // else if
            else
            {
                unsupported = 1;
            } // else

            ctx->dwords[0] = (uint32) MOJOSHADER_USAGE_UNKNOWN;
            ctx->dwords[1] = 0;
        } // else if

        else if (regtype == REG_TYPE_TEXTURE)
        {
            const uint32 usage = (token & 0xF);
            const uint32 index = ((token >> 16) & 0xF);
            if (usage == MOJOSHADER_USAGE_TEXCOORD)
            {
                if (index > 7)
                    fail(ctx, "DCL texcoord usage must have 0-7 index");
            } // if
            else if (usage == MOJOSHADER_USAGE_COLOR)
            {
                if (index != 0)
                    fail(ctx, "DCL color usage must have 0 index");
            } // else if
            else
            {
                fail(ctx, "Invalid DCL texture usage");
            } // else

            reserved_mask = 0x7FF0FFE0;
            ctx->dwords[0] = usage;
            ctx->dwords[1] = index;
        } // else if

        else if (regtype == REG_TYPE_SAMPLER)
        {
            const uint32 ttype = ((token >> 27) & 0xF);
            if (!valid_texture_type(ttype))
                fail(ctx, "unknown sampler texture type");
            reserved_mask = 0x7FFFFFF;
            ctx->dwords[0] = ttype;
        } // else if

        else
        {
            unsupported = 1;
        } // else
    } // if

    else if ( (shader_is_pixel(ctx)) && (shader_version_atleast(ctx, 2, 0)) )
    {
        if (regtype == REG_TYPE_INPUT)
        {
            ctx->dwords[0] = (uint32) MOJOSHADER_USAGE_COLOR;
            ctx->dwords[1] = regnum;
            reserved_mask = 0x7FFFFFFF;
        } // if
        else if (regtype == REG_TYPE_TEXTURE)
        {
            ctx->dwords[0] = (uint32) MOJOSHADER_USAGE_TEXCOORD;
            ctx->dwords[1] = regnum;
            reserved_mask = 0x7FFFFFFF;
        } // else if
        else if (regtype == REG_TYPE_SAMPLER)
        {
            const uint32 ttype = ((token >> 27) & 0xF);
            if (!valid_texture_type(ttype))
                fail(ctx, "unknown sampler texture type");
            reserved_mask = 0x7FFFFFF;
            ctx->dwords[0] = ttype;
        } // else if
        else
        {
            unsupported = 1;
        } // else
    } // if

    else if ( (shader_is_vertex(ctx)) && (shader_version_atleast(ctx, 3, 0)) )
    {
        if ((regtype == REG_TYPE_INPUT) || (regtype == REG_TYPE_OUTPUT))
        {
            const uint32 usage = (token & 0xF);
            const uint32 index = ((token >> 16) & 0xF);
            reserved_mask = 0x7FF0FFE0;
            ctx->dwords[0] = usage;
            ctx->dwords[1] = index;
        } // if
        else if (regtype == REG_TYPE_TEXTURE)
        {
            const uint32 usage = (token & 0xF);
            const uint32 index = ((token >> 16) & 0xF);
            if (usage == MOJOSHADER_USAGE_TEXCOORD)
            {
                if (index > 7)
                    fail(ctx, "DCL texcoord usage must have 0-7 index");
            } // if
            else if (usage == MOJOSHADER_USAGE_COLOR)
            {
                if (index != 0)
                    fail(ctx, "DCL texcoord usage must have 0 index");
            } // else if
            else
                fail(ctx, "Invalid DCL texture usage");

            reserved_mask = 0x7FF0FFE0;
            ctx->dwords[0] = usage;
            ctx->dwords[1] = index;
        } // else if
        else if (regtype == REG_TYPE_SAMPLER)
        {
            const uint32 ttype = ((token >> 27) & 0xF);
            if (!valid_texture_type(ttype))
                fail(ctx, "Unknown sampler texture type");
            reserved_mask = 0x6FFFFFFF;
            ctx->dwords[0] = ttype;
        } // else if
        else
        {
            unsupported = 1;
        } // else
    } // else if

    else if ( (shader_is_vertex(ctx)) && (shader_version_atleast(ctx, 1, 1)) )
    {
        if (regtype == REG_TYPE_INPUT)
        {
            const uint32 usage = (token & 0xF);
            const uint32 index = ((token >> 16) & 0xF);
            reserved_mask = 0x7FF0FFE0;
            ctx->dwords[0] = usage;
            ctx->dwords[1] = index;
        } // if
        else
        {
            unsupported = 1;
        } // else
    } // else if

    else
    {
        unsupported = 1;
    } // else

    if (unsupported)
        fail(ctx, "invalid DCL register type for this shader model");

    if ((token & reserved_mask) != 0)
        fail(ctx, "reserved bits in DCL dword aren't zero");

    return 3;
} // parse_args_DCL


static int parse_args_D(Context *ctx)
{
    int retval = 1;
    retval += parse_destination_token(ctx, &ctx->dest_arg);
    return retval;
} // parse_args_D


static int parse_args_S(Context *ctx)
{
    int retval = 1;
    retval += parse_source_token(ctx, &ctx->source_args[0]);
    return retval;
} // parse_args_S


static int parse_args_SS(Context *ctx)
{
    int retval = 1;
    retval += parse_source_token(ctx, &ctx->source_args[0]);
    retval += parse_source_token(ctx, &ctx->source_args[1]);
    return retval;
} // parse_args_SS


static int parse_args_DS(Context *ctx)
{
    int retval = 1;
    retval += parse_destination_token(ctx, &ctx->dest_arg);
    retval += parse_source_token(ctx, &ctx->source_args[0]);
    return retval;
} // parse_args_DS


static int parse_args_DSS(Context *ctx)
{
    int retval = 1;
    retval += parse_destination_token(ctx, &ctx->dest_arg);
    retval += parse_source_token(ctx, &ctx->source_args[0]);
    retval += parse_source_token(ctx, &ctx->source_args[1]);
    return retval;
} // parse_args_DSS


static int parse_args_DSSS(Context *ctx)
{
    int retval = 1;
    retval += parse_destination_token(ctx, &ctx->dest_arg);
    retval += parse_source_token(ctx, &ctx->source_args[0]);
    retval += parse_source_token(ctx, &ctx->source_args[1]);
    retval += parse_source_token(ctx, &ctx->source_args[2]);
    return retval;
} // parse_args_DSSS


static int parse_args_DSSSS(Context *ctx)
{
    int retval = 1;
    retval += parse_destination_token(ctx, &ctx->dest_arg);
    retval += parse_source_token(ctx, &ctx->source_args[0]);
    retval += parse_source_token(ctx, &ctx->source_args[1]);
    retval += parse_source_token(ctx, &ctx->source_args[2]);
    retval += parse_source_token(ctx, &ctx->source_args[3]);
    return retval;
} // parse_args_DSSSS


static int parse_args_SINCOS(Context *ctx)
{
    // this opcode needs extra registers for sm2 and lower.
    if (!shader_version_atleast(ctx, 3, 0))
        return parse_args_DSSS(ctx);
    return parse_args_DS(ctx);
} // parse_args_SINCOS


static int parse_args_TEXCRD(Context *ctx)
{
    // added extra register in ps_1_4.
    if (shader_version_atleast(ctx, 1, 4))
        return parse_args_DS(ctx);
    return parse_args_D(ctx);
} // parse_args_TEXCRD


static int parse_args_TEXLD(Context *ctx)
{
    // different registers in px_1_3, ps_1_4, and ps_2_0!
    if (shader_version_atleast(ctx, 2, 0))
        return parse_args_DSS(ctx);
    else if (shader_version_atleast(ctx, 1, 4))
        return parse_args_DS(ctx);
    return parse_args_D(ctx);
} // parse_args_TEXLD


// State machine functions...

static ConstantsList *alloc_constant_listitem(Context *ctx)
{
    ConstantsList *item = (ConstantsList *) Malloc(ctx, sizeof (ConstantsList));
    if (item == NULL)
        return NULL;

    memset(&item->constant, '\0', sizeof (MOJOSHADER_constant));
    item->next = ctx->constants;
    ctx->constants = item;
    ctx->constant_count++;

    return item;
} // alloc_constant_listitem


static void state_DEF(Context *ctx)
{
    const RegisterType regtype = ctx->dest_arg.regtype;
    const int regnum = ctx->dest_arg.regnum;

    // !!! FIXME: fail if same register is defined twice.

    if (ctx->instruction_count != 0)
        fail(ctx, "DEF token must come before any instructions");
    else if (regtype != REG_TYPE_CONST)
        fail(ctx, "DEF token using invalid register");
    else
    {
        ConstantsList *item = alloc_constant_listitem(ctx);
        if (item != NULL)
        {
            item->constant.index = regnum;
            item->constant.type = MOJOSHADER_UNIFORM_FLOAT;
            memcpy(item->constant.value.f, ctx->dwords,
                   sizeof (item->constant.value.f));
            set_defined_register(ctx, regtype, regnum);
        } // if
    } // else
} // state_DEF

static void state_DEFI(Context *ctx)
{
    const RegisterType regtype = ctx->dest_arg.regtype;
    const int regnum = ctx->dest_arg.regnum;

    // !!! FIXME: fail if same register is defined twice.

    if (ctx->instruction_count != 0)
        fail(ctx, "DEFI token must come before any instructions");
    else if (regtype != REG_TYPE_CONSTINT)
        fail(ctx, "DEFI token using invalid register");
    else
    {
        ConstantsList *item = alloc_constant_listitem(ctx);
        if (item != NULL)
        {
            item->constant.index = regnum;
            item->constant.type = MOJOSHADER_UNIFORM_INT;
            memcpy(item->constant.value.i, ctx->dwords,
                   sizeof (item->constant.value.i));

            set_defined_register(ctx, regtype, regnum);
        } // if
    } // else
} // state_DEFI

static void state_DEFB(Context *ctx)
{
    const RegisterType regtype = ctx->dest_arg.regtype;
    const int regnum = ctx->dest_arg.regnum;

    // !!! FIXME: fail if same register is defined twice.

    if (ctx->instruction_count != 0)
        fail(ctx, "DEFB token must come before any instructions");
    else if (regtype != REG_TYPE_CONSTBOOL)
        fail(ctx, "DEFB token using invalid register");
    else
    {
        ConstantsList *item = alloc_constant_listitem(ctx);
        if (item != NULL)
        {
            item->constant.index = regnum;
            item->constant.type = MOJOSHADER_UNIFORM_BOOL;
            item->constant.value.b = ctx->dwords[0] ? 1 : 0;
            set_defined_register(ctx, regtype, regnum);
        } // if
    } // else
} // state_DEFB

static void state_DCL(Context *ctx)
{
    const DestArgInfo *arg = &ctx->dest_arg;
    const RegisterType regtype = arg->regtype;
    const int regnum = arg->regnum;
    const int wmask = arg->writemask;
    const int mods = arg->result_mod;

    // parse_args_DCL() does a lot of state checking before we get here.

    // !!! FIXME: apparently vs_3_0 can use sampler registers now.
    // !!! FIXME:  (but only s0 through s3, not all 16 of them.)

    if (ctx->instruction_count != 0)
        fail(ctx, "DCL token must come before any instructions");

    else if (shader_is_vertex(ctx))
    {
        if (regtype == REG_TYPE_SAMPLER)
            add_sampler(ctx, regnum, (TextureType) ctx->dwords[0], 0);
        else
        {
            const MOJOSHADER_usage usage = (const MOJOSHADER_usage) ctx->dwords[0];
            const int index = ctx->dwords[1];
            if (usage >= MOJOSHADER_USAGE_TOTAL)
            {
                fail(ctx, "unknown DCL usage");
                return;
            } // if
            add_attribute_register(ctx, regtype, regnum, usage, index, wmask, mods);
        } // else
    } // if

    else if (shader_is_pixel(ctx))
    {
        if (regtype == REG_TYPE_SAMPLER)
            add_sampler(ctx, regnum, (TextureType) ctx->dwords[0], 0);
        else
        {
            const MOJOSHADER_usage usage = (MOJOSHADER_usage) ctx->dwords[0];
            const int index = ctx->dwords[1];
            add_attribute_register(ctx, regtype, regnum, usage, index, wmask, mods);
        } // else
    } // else if

    else
    {
        fail(ctx, "unsupported shader type."); // should be caught elsewhere.
        return;
    } // else

    set_defined_register(ctx, regtype, regnum);
} // state_DCL

static void state_TEXCRD(Context *ctx)
{
    if (shader_version_atleast(ctx, 2, 0))
        fail(ctx, "TEXCRD in Shader Model >= 2.0");  // apparently removed.
} // state_TEXCRD

static void state_FRC(Context *ctx)
{
    const DestArgInfo *dst = &ctx->dest_arg;

    if (dst->result_mod & MOD_SATURATE)  // according to msdn...
        fail(ctx, "FRC destination can't use saturate modifier");

    else if (!shader_version_atleast(ctx, 2, 0))
    {
        if (!writemask_y(dst->writemask) && !writemask_xy(dst->writemask))
            fail(ctx, "FRC writemask must be .y or .xy for shader model 1.x");
    } // else if
} // state_FRC


// replicate the matrix registers to source args. The D3D profile will
//  only use the one legitimate argument, but this saves other profiles
//  from having to build this.
static void srcarg_matrix_replicate(Context *ctx, const int idx,
                                       const int rows)
{
    int i;
    SourceArgInfo *src = &ctx->source_args[idx];
    SourceArgInfo *dst = &ctx->source_args[idx+1];
    for (i = 0; i < (rows-1); i++, dst++)
    {
        memcpy(dst, src, sizeof (SourceArgInfo));
        dst->regnum += (i + 1);
        set_used_register(ctx, dst->regtype, dst->regnum, 0);
    } // for
} // srcarg_matrix_replicate

static void state_M4X4(Context *ctx)
{
    const DestArgInfo *info = &ctx->dest_arg;
    if (!writemask_xyzw(info->writemask))
        fail(ctx, "M4X4 writemask must be full");

// !!! FIXME: MSDN:
//The xyzw (default) mask is required for the destination register. Negate and swizzle modifiers are allowed for src0, but not for src1.
//Swizzle and negate modifiers are invalid for the src0 register. The dest and src0 registers cannot be the same.

    srcarg_matrix_replicate(ctx, 1, 4);
} // state_M4X4

static void state_M4X3(Context *ctx)
{
    const DestArgInfo *info = &ctx->dest_arg;
    if (!writemask_xyz(info->writemask))
        fail(ctx, "M4X3 writemask must be .xyz");

// !!! FIXME: MSDN stuff

    srcarg_matrix_replicate(ctx, 1, 3);
} // state_M4X3

static void state_M3X4(Context *ctx)
{
    const DestArgInfo *info = &ctx->dest_arg;
    if (!writemask_xyzw(info->writemask))
        fail(ctx, "M3X4 writemask must be .xyzw");

// !!! FIXME: MSDN stuff

    srcarg_matrix_replicate(ctx, 1, 4);
} // state_M3X4

static void state_M3X3(Context *ctx)
{
    const DestArgInfo *info = &ctx->dest_arg;
    if (!writemask_xyz(info->writemask))
        fail(ctx, "M3X3 writemask must be .xyz");

// !!! FIXME: MSDN stuff

    srcarg_matrix_replicate(ctx, 1, 3);
} // state_M3X3

static void state_M3X2(Context *ctx)
{
    const DestArgInfo *info = &ctx->dest_arg;
    if (!writemask_xy(info->writemask))
        fail(ctx, "M3X2 writemask must be .xy");

// !!! FIXME: MSDN stuff

    srcarg_matrix_replicate(ctx, 1, 2);
} // state_M3X2

static void state_RET(Context *ctx)
{
    // MSDN all but says that assembly shaders are more or less serialized
    //  HLSL functions, and a RET means you're at the end of one, unlike how
    //  most CPUs would behave. This is actually really helpful,
    //  since we can use high-level constructs and not a mess of GOTOs,
    //  which is a godsend for GLSL...this also means we can consider things
    //  like a LOOP without a matching ENDLOOP within a label's section as
    //  an error.
    if (ctx->loops > 0)
        fail(ctx, "LOOP without ENDLOOP");
    if (ctx->reps > 0)
        fail(ctx, "REP without ENDREP");
} // state_RET

static void check_label_register(Context *ctx, int arg, const char *opcode)
{
    const SourceArgInfo *info = &ctx->source_args[arg];
    const RegisterType regtype = info->regtype;
    const int regnum = info->regnum;

    if (regtype != REG_TYPE_LABEL)
        failf(ctx, "%s with a non-label register specified", opcode);
    if (!shader_version_atleast(ctx, 2, 0))
        failf(ctx, "%s not supported in Shader Model 1", opcode);
    if ((shader_version_atleast(ctx, 2, 255)) && (regnum > 2047))
        fail(ctx, "label register number must be <= 2047");
    if (regnum > 15)
        fail(ctx, "label register number must be <= 15");
} // check_label_register

static void state_LABEL(Context *ctx)
{
    if (ctx->previous_opcode != OPCODE_RET)
        fail(ctx, "LABEL not followed by a RET");
    check_label_register(ctx, 0, "LABEL");
    set_defined_register(ctx, REG_TYPE_LABEL, ctx->source_args[0].regnum);
} // state_LABEL

static void check_call_loop_wrappage(Context *ctx, const int regnum)
{
    // msdn says subroutines inherit aL register if you're in a loop when
    //  you call, and further more _if you ever call this function in a loop,
    //  it must always be called in a loop_. So we'll just pass our loop
    //  variable as a function parameter in those cases.

    const int current_usage = (ctx->loops > 0) ? 1 : -1;
    RegisterList *reg = reglist_find(&ctx->used_registers, REG_TYPE_LABEL, regnum);

    if (reg == NULL)
        fail(ctx, "Invalid label for CALL");
    else if (reg->misc == 0)
        reg->misc = current_usage;
    else if (reg->misc != current_usage)
    {
        if (current_usage == 1)
            fail(ctx, "CALL to this label must be wrapped in LOOP/ENDLOOP");
        else
            fail(ctx, "CALL to this label must not be wrapped in LOOP/ENDLOOP");
    } // else if
} // check_call_loop_wrappage

static void state_CALL(Context *ctx)
{
    check_label_register(ctx, 0, "CALL");
    check_call_loop_wrappage(ctx, ctx->source_args[0].regnum);
} // state_CALL

static void state_CALLNZ(Context *ctx)
{
    const RegisterType regtype = ctx->source_args[1].regtype;
    if ((regtype != REG_TYPE_CONSTBOOL) && (regtype != REG_TYPE_PREDICATE))
        fail(ctx, "CALLNZ argument isn't constbool or predicate register");
    check_label_register(ctx, 0, "CALLNZ");
    check_call_loop_wrappage(ctx, ctx->source_args[0].regnum);
} // state_CALLNZ

static void state_MOVA(Context *ctx)
{
    if (ctx->dest_arg.regtype != REG_TYPE_ADDRESS)
        fail(ctx, "MOVA argument isn't address register");
} // state_MOVA

static void state_RCP(Context *ctx)
{
    if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "RCP without replicate swizzzle");
} // state_RCP

static void state_LOOP(Context *ctx)
{
    if (ctx->source_args[0].regtype != REG_TYPE_LOOP)
        fail(ctx, "LOOP argument isn't loop register");
    else if (ctx->source_args[1].regtype != REG_TYPE_CONSTINT)
        fail(ctx, "LOOP argument isn't constint register");
    else
        ctx->loops++;
} // state_LOOP

static void state_ENDLOOP(Context *ctx)
{
    // !!! FIXME: check that we aren't straddling an IF block.
    if (ctx->loops <= 0)
        fail(ctx, "ENDLOOP without LOOP");
    ctx->loops--;
} // state_ENDLOOP

static void state_BREAKP(Context *ctx)
{
    const RegisterType regtype = ctx->source_args[0].regtype;
    if (regtype != REG_TYPE_PREDICATE)
        fail(ctx, "BREAKP argument isn't predicate register");
    else if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "BREAKP without replicate swizzzle");
    else if ((ctx->loops == 0) && (ctx->reps == 0))
        fail(ctx, "BREAKP outside LOOP/ENDLOOP or REP/ENDREP");
} // state_BREAKP

static void state_BREAK(Context *ctx)
{
    if ((ctx->loops == 0) && (ctx->reps == 0))
        fail(ctx, "BREAK outside LOOP/ENDLOOP or REP/ENDREP");
} // state_BREAK

static void state_SETP(Context *ctx)
{
    const RegisterType regtype = ctx->dest_arg.regtype;
    if (regtype != REG_TYPE_PREDICATE)
        fail(ctx, "SETP argument isn't predicate register");
} // state_SETP

static void state_REP(Context *ctx)
{
    const RegisterType regtype = ctx->source_args[0].regtype;
    if (regtype != REG_TYPE_CONSTINT)
        fail(ctx, "REP argument isn't constint register");

    ctx->reps++;
    if (ctx->reps > ctx->max_reps)
        ctx->max_reps = ctx->reps;
} // state_REP

static void state_ENDREP(Context *ctx)
{
    // !!! FIXME: check that we aren't straddling an IF block.
    if (ctx->reps <= 0)
        fail(ctx, "ENDREP without REP");
    ctx->reps--;
} // state_ENDREP

static void state_CMP(Context *ctx)
{
    ctx->cmps++;

    // extra limitations for ps <= 1.4 ...
    if (!shader_version_atleast(ctx, 1, 4))
    {
        int i;
        const DestArgInfo *dst = &ctx->dest_arg;
        const RegisterType dregtype = dst->regtype;
        const int dregnum = dst->regnum;

        if (ctx->cmps > 3)
            fail(ctx, "only 3 CMP instructions allowed in this shader model");

        for (i = 0; i < 3; i++)
        {
            const SourceArgInfo *src = &ctx->source_args[i];
            const RegisterType sregtype = src->regtype;
            const int sregnum = src->regnum;
            if ((dregtype == sregtype) && (dregnum == sregnum))
                fail(ctx, "CMP dest can't match sources in this shader model");
        } // for

        ctx->instruction_count++;  // takes an extra slot in ps_1_2 and _3.
    } // if
} // state_CMP

static void state_DP4(Context *ctx)
{
    // extra limitations for ps <= 1.4 ...
    if (!shader_version_atleast(ctx, 1, 4))
        ctx->instruction_count++;  // takes an extra slot in ps_1_2 and _3.
} // state_DP4

static void state_CND(Context *ctx)
{
    // apparently it was removed...it's not in the docs past ps_1_4 ...
    if (shader_version_atleast(ctx, 2, 0))
        fail(ctx, "CND not allowed in this shader model");

    // extra limitations for ps <= 1.4 ...
    else if (!shader_version_atleast(ctx, 1, 4))
    {
        const SourceArgInfo *src = &ctx->source_args[0];
        if ((src->regtype != REG_TYPE_TEMP) || (src->regnum != 0) ||
            (src->swizzle != 0xFF))
        {
            fail(ctx, "CND src must be r0.a in this shader model");
        } // if
    } // if
} // state_CND

static void state_POW(Context *ctx)
{
    if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "POW src0 must have replicate swizzle");
    else if (!replicate_swizzle(ctx->source_args[1].swizzle))
        fail(ctx, "POW src1 must have replicate swizzle");
} // state_POW

static void state_LOG(Context *ctx)
{
    if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "LOG src0 must have replicate swizzle");
} // state_LOG

static void state_LOGP(Context *ctx)
{
    if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "LOGP src0 must have replicate swizzle");
} // state_LOGP

static void state_SINCOS(Context *ctx)
{
    const DestArgInfo *dst = &ctx->dest_arg;
    const int mask = dst->writemask;
    if (!writemask_x(mask) && !writemask_y(mask) && !writemask_xy(mask))
        fail(ctx, "SINCOS write mask must be .x or .y or .xy");

    else if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "SINCOS src0 must have replicate swizzle");

    else if (dst->result_mod & MOD_SATURATE)  // according to msdn...
        fail(ctx, "SINCOS destination can't use saturate modifier");

    // this opcode needs extra registers, with extra limitations, for <= sm2.
    else if (!shader_version_atleast(ctx, 3, 0))
    {
        int i;
        for (i = 1; i < 3; i++)
        {
            if (ctx->source_args[i].regtype != REG_TYPE_CONST)
            {
                failf(ctx, "SINCOS src%d must be constfloat", i);
                return;
            } // if
        } // for

        if (ctx->source_args[1].regnum == ctx->source_args[2].regnum)
            fail(ctx, "SINCOS src1 and src2 must be different registers");
    } // if
} // state_SINCOS

static void state_IF(Context *ctx)
{
    const RegisterType regtype = ctx->source_args[0].regtype;
    if ((regtype != REG_TYPE_PREDICATE) && (regtype != REG_TYPE_CONSTBOOL))
        fail(ctx, "IF src0 must be CONSTBOOL or PREDICATE");
    // !!! FIXME: track if nesting depth.
} // state_IF

static void state_IFC(Context *ctx)
{
    if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "IFC src0 must have replicate swizzle");
    else if (!replicate_swizzle(ctx->source_args[1].swizzle))
        fail(ctx, "IFC src1 must have replicate swizzle");
    // !!! FIXME: track if nesting depth.
} // state_IFC

static void state_BREAKC(Context *ctx)
{
    if (!replicate_swizzle(ctx->source_args[0].swizzle))
        fail(ctx, "BREAKC src1 must have replicate swizzle");
    else if (!replicate_swizzle(ctx->source_args[1].swizzle))
        fail(ctx, "BREAKC src2 must have replicate swizzle");
    else if ((ctx->loops == 0) && (ctx->reps == 0))
        fail(ctx, "BREAKC outside LOOP/ENDLOOP or REP/ENDREP");
} // state_BREAKC

static void state_TEXKILL(Context *ctx)
{
    // The MSDN docs say this should be a source arg, but the driver docs
    //  say it's a dest arg. That's annoying.
    const DestArgInfo *info = &ctx->dest_arg;
    const RegisterType regtype = info->regtype;
    if (!writemask_xyzw(info->writemask))
        fail(ctx, "TEXKILL writemask must be .xyzw");
    else if ((regtype != REG_TYPE_TEMP) && (regtype != REG_TYPE_TEXTURE))
        fail(ctx, "TEXKILL must use a temp or texture register");

    // !!! FIXME: "If a temporary register is used, all components must have been previously written."
    // !!! FIXME: "If a texture register is used, all components that are read must have been declared."
    // !!! FIXME: there are further limitations in ps_1_3 and earlier.
} // state_TEXKILL

// Some rules that apply to some of the fruity ps_1_1 texture opcodes...
static void state_texops(Context *ctx, const char *opcode,
                         const int dims, const int texbem)
{
    const DestArgInfo *dst = &ctx->dest_arg;
    const SourceArgInfo *src = &ctx->source_args[0];
    if (dst->regtype != REG_TYPE_TEXTURE)
        failf(ctx, "%s destination must be a texture register", opcode);
    if (src->regtype != REG_TYPE_TEXTURE)
        failf(ctx, "%s source must be a texture register", opcode);
    if (src->regnum >= dst->regnum)  // so says MSDN.
        failf(ctx, "%s dest must be a higher register than source", opcode);

    if (dims)
    {
        TextureType ttyp = (dims == 2) ? TEXTURE_TYPE_2D : TEXTURE_TYPE_CUBE;
        add_sampler(ctx, dst->regnum, ttyp, texbem);
    } // if

    add_attribute_register(ctx, REG_TYPE_TEXTURE, dst->regnum,
                           MOJOSHADER_USAGE_TEXCOORD, dst->regnum, 0xF, 0);

    // Strictly speaking, there should be a TEX opcode prior to this call that
    //  should fill in this metadata, but I'm not sure that's required for the
    //  shader to assemble in D3D, so we'll do this so we don't fail with a
    //  cryptic error message even if the developer didn't do the TEX.
    add_attribute_register(ctx, REG_TYPE_TEXTURE, src->regnum,
                           MOJOSHADER_USAGE_TEXCOORD, src->regnum, 0xF, 0);
} // state_texops

static void state_texbem(Context *ctx, const char *opcode)
{
    // The TEXBEM equasion, according to MSDN:
    //u' = TextureCoordinates(stage m)u + D3DTSS_BUMPENVMAT00(stage m)*t(n)R
    //         + D3DTSS_BUMPENVMAT10(stage m)*t(n)G
    //v' = TextureCoordinates(stage m)v + D3DTSS_BUMPENVMAT01(stage m)*t(n)R
    //         + D3DTSS_BUMPENVMAT11(stage m)*t(n)G
    //t(m)RGBA = TextureSample(stage m)
    //
    // ...TEXBEML adds this at the end:
    //t(m)RGBA = t(m)RGBA * [(t(n)B * D3DTSS_BUMPENVLSCALE(stage m)) +
    //           D3DTSS_BUMPENVLOFFSET(stage m)]

    if (shader_version_atleast(ctx, 1, 4))
        failf(ctx, "%s opcode not available after Shader Model 1.3", opcode);

    if (!shader_version_atleast(ctx, 1, 2))
    {
        if (ctx->source_args[0].src_mod == SRCMOD_SIGN)
            failf(ctx, "%s forbids _bx2 on source reg before ps_1_2", opcode);
    } // if

    // !!! FIXME: MSDN:
    // !!! FIXME: Register data that has been read by a texbem
    // !!! FIXME:  or texbeml instruction cannot be read later,
    // !!! FIXME:  except by another texbem or texbeml.

    state_texops(ctx, opcode, 2, 1);
} // state_texbem

static void state_TEXBEM(Context *ctx)
{
    state_texbem(ctx, "TEXBEM");
} // state_TEXBEM

static void state_TEXBEML(Context *ctx)
{
    state_texbem(ctx, "TEXBEML");
} // state_TEXBEML

static void state_TEXM3X2PAD(Context *ctx)
{
    if (shader_version_atleast(ctx, 1, 4))
        fail(ctx, "TEXM3X2PAD opcode not available after Shader Model 1.3");
    state_texops(ctx, "TEXM3X2PAD", 0, 0);
    // !!! FIXME: check for correct opcode existance and order more rigorously?
    ctx->texm3x2pad_src0 = ctx->source_args[0].regnum;
    ctx->texm3x2pad_dst0 = ctx->dest_arg.regnum;
} // state_TEXM3X2PAD

static void state_TEXM3X2TEX(Context *ctx)
{
    if (shader_version_atleast(ctx, 1, 4))
        fail(ctx, "TEXM3X2TEX opcode not available after Shader Model 1.3");
    if (ctx->texm3x2pad_dst0 == -1)
        fail(ctx, "TEXM3X2TEX opcode without matching TEXM3X2PAD");
    // !!! FIXME: check for correct opcode existance and order more rigorously?
    state_texops(ctx, "TEXM3X2TEX", 2, 0);
    ctx->reset_texmpad = 1;

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      ctx->dest_arg.regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);

    // A samplermap might change this to something nonsensical.
    if (ttype != TEXTURE_TYPE_2D)
        fail(ctx, "TEXM3X2TEX needs a 2D sampler");
} // state_TEXM3X2TEX

static void state_TEXM3X3PAD(Context *ctx)
{
    if (shader_version_atleast(ctx, 1, 4))
        fail(ctx, "TEXM3X2TEX opcode not available after Shader Model 1.3");
    state_texops(ctx, "TEXM3X3PAD", 0, 0);

    // !!! FIXME: check for correct opcode existance and order more rigorously?
    if (ctx->texm3x3pad_dst0 == -1)
    {
        ctx->texm3x3pad_src0 = ctx->source_args[0].regnum;
        ctx->texm3x3pad_dst0 = ctx->dest_arg.regnum;
    } // if
    else if (ctx->texm3x3pad_dst1 == -1)
    {
        ctx->texm3x3pad_src1 = ctx->source_args[0].regnum;
        ctx->texm3x3pad_dst1 = ctx->dest_arg.regnum;
    } // else
} // state_TEXM3X3PAD

static void state_texm3x3(Context *ctx, const char *opcode, const int dims)
{
    // !!! FIXME: check for correct opcode existance and order more rigorously?
    if (shader_version_atleast(ctx, 1, 4))
        failf(ctx, "%s opcode not available after Shader Model 1.3", opcode);
    if (ctx->texm3x3pad_dst1 == -1)
        failf(ctx, "%s opcode without matching TEXM3X3PADs", opcode);
    state_texops(ctx, opcode, dims, 0);
    ctx->reset_texmpad = 1;

    RegisterList *sreg = reglist_find(&ctx->samplers, REG_TYPE_SAMPLER,
                                      ctx->dest_arg.regnum);
    const TextureType ttype = (TextureType) (sreg ? sreg->index : 0);

    // A samplermap might change this to something nonsensical.
    if ((ttype != TEXTURE_TYPE_VOLUME) && (ttype != TEXTURE_TYPE_CUBE))
        failf(ctx, "%s needs a 3D or Cubemap sampler", opcode);
} // state_texm3x3

static void state_TEXM3X3(Context *ctx)
{
    if (!shader_version_atleast(ctx, 1, 2))
        fail(ctx, "TEXM3X3 opcode not available in Shader Model 1.1");
    state_texm3x3(ctx, "TEXM3X3", 0);
} // state_TEXM3X3

static void state_TEXM3X3TEX(Context *ctx)
{
    state_texm3x3(ctx, "TEXM3X3TEX", 3);
} // state_TEXM3X3TEX

static void state_TEXM3X3SPEC(Context *ctx)
{
    state_texm3x3(ctx, "TEXM3X3SPEC", 3);
    if (ctx->source_args[1].regtype != REG_TYPE_CONST)
        fail(ctx, "TEXM3X3SPEC final arg must be a constant register");
} // state_TEXM3X3SPEC

static void state_TEXM3X3VSPEC(Context *ctx)
{
    state_texm3x3(ctx, "TEXM3X3VSPEC", 3);
} // state_TEXM3X3VSPEC


static void state_TEXLD(Context *ctx)
{
    if (shader_version_atleast(ctx, 2, 0))
    {
        const SourceArgInfo *src0 = &ctx->source_args[0];
        const SourceArgInfo *src1 = &ctx->source_args[1];

        // !!! FIXME: verify texldp restrictions:
        //http://msdn.microsoft.com/en-us/library/bb206221(VS.85).aspx
        // !!! FIXME: ...and texldb, too.
        //http://msdn.microsoft.com/en-us/library/bb206217(VS.85).aspx

        //const RegisterType rt0 = src0->regtype;

        // !!! FIXME: msdn says it has to be temp, but Microsoft's HLSL
        // !!! FIXME:  compiler is generating code that uses oC0 for a dest.
        //if (ctx->dest_arg.regtype != REG_TYPE_TEMP)
        //    fail(ctx, "TEXLD dest must be a temp register");

        // !!! FIXME: this can be an REG_TYPE_INPUT, DCL'd to TEXCOORD.
        //else if ((rt0 != REG_TYPE_TEXTURE) && (rt0 != REG_TYPE_TEMP))
        //    fail(ctx, "TEXLD src0 must be texture or temp register");
        //else

        if (src0->src_mod != SRCMOD_NONE)
            fail(ctx, "TEXLD src0 must have no modifiers");
        else if (src1->regtype != REG_TYPE_SAMPLER)
            fail(ctx, "TEXLD src1 must be sampler register");
        else if (src1->src_mod != SRCMOD_NONE)
            fail(ctx, "TEXLD src1 must have no modifiers");
        else if ( (ctx->instruction_controls != CONTROL_TEXLD) &&
                  (ctx->instruction_controls != CONTROL_TEXLDP) &&
                  (ctx->instruction_controls != CONTROL_TEXLDB) )
        {
            fail(ctx, "TEXLD has unknown control bits");
        } // else if

        // Shader Model 3 added swizzle support to this opcode.
        if (!shader_version_atleast(ctx, 3, 0))
        {
            if (!no_swizzle(src0->swizzle))
                fail(ctx, "TEXLD src0 must not swizzle");
            else if (!no_swizzle(src1->swizzle))
                fail(ctx, "TEXLD src1 must not swizzle");
        } // if

        if ( ((TextureType) ctx->source_args[1].regnum) == TEXTURE_TYPE_CUBE )
            ctx->instruction_count += 3;
    } // if

    else if (shader_version_atleast(ctx, 1, 4))
    {
        // !!! FIXME: checks for ps_1_4 version here...
    } // else if

    else
    {
        // !!! FIXME: add (other?) checks for ps_1_1 version here...
        const DestArgInfo *info = &ctx->dest_arg;
        const int sampler = info->regnum;
        if (info->regtype != REG_TYPE_TEXTURE)
            fail(ctx, "TEX param must be a texture register");
        add_sampler(ctx, sampler, TEXTURE_TYPE_2D, 0);
        add_attribute_register(ctx, REG_TYPE_TEXTURE, sampler,
                               MOJOSHADER_USAGE_TEXCOORD, sampler, 0xF, 0);
    } // else
} // state_TEXLD

static void state_TEXLDL(Context *ctx)
{
    if (!shader_version_atleast(ctx, 3, 0))
        fail(ctx, "TEXLDL in version < Shader Model 3.0");
    else if (ctx->source_args[1].regtype != REG_TYPE_SAMPLER)
        fail(ctx, "TEXLDL src1 must be sampler register");
    else
    {
        if ( ((TextureType) ctx->source_args[1].regnum) == TEXTURE_TYPE_CUBE )
            ctx->instruction_count += 3;
    } // else
} // state_TEXLDL

static void state_DP2ADD(Context *ctx)
{
    if (!replicate_swizzle(ctx->source_args[2].swizzle))
        fail(ctx, "DP2ADD src2 must have replicate swizzle");
} // state_DP2ADD


// Lookup table for instruction opcodes...
typedef struct
{
    const char *opcode_string;
    int slots;  // number of instruction slots this opcode eats.
    MOJOSHADER_shaderType shader_types;  // mask of types that can use opcode.
    args_function parse_args;
    state_function state;
    emit_function emitter[STATICARRAYLEN(profiles)];
} Instruction;

// These have to be in the right order! This array is indexed by the value
//  of the instruction token.
static const Instruction instructions[] =
{
    #define INSTRUCTION_STATE(op, opstr, slots, a, t) { \
        opstr, slots, t, parse_args_##a, state_##op, PROFILE_EMITTERS(op) \
    },

    #define INSTRUCTION(op, opstr, slots, a, t) { \
        opstr, slots, t, parse_args_##a, 0, PROFILE_EMITTERS(op) \
    },

    #define MOJOSHADER_DO_INSTRUCTION_TABLE 1
    #include "mojoshader_internal.h"
    #undef MOJOSHADER_DO_INSTRUCTION_TABLE

    #undef INSTRUCTION
    #undef INSTRUCTION_STATE
};


// parse various token types...

static int parse_instruction_token(Context *ctx)
{
    int retval = 0;
    const int start_position = ctx->current_position;
    const uint32 *start_tokens = ctx->tokens;
    const uint32 start_tokencount = ctx->tokencount;
    const uint32 token = SWAP32(*(ctx->tokens));
    const uint32 opcode = (token & 0xFFFF);
    const uint32 controls = ((token >> 16) & 0xFF);
    const uint32 insttoks = ((token >> 24) & 0x0F);
    const int coissue = (token & 0x40000000) ? 1 : 0;
    const int predicated = (token & 0x10000000) ? 1 : 0;

    if ( opcode >= (sizeof (instructions) / sizeof (instructions[0])) )
        return 0;  // not an instruction token, or just not handled here.

    const Instruction *instruction = &instructions[opcode];
    const emit_function emitter = instruction->emitter[ctx->profileid];

    if ((token & 0x80000000) != 0)
        fail(ctx, "instruction token high bit must be zero.");  // so says msdn.

    if (instruction->opcode_string == NULL)
    {
        fail(ctx, "Unknown opcode.");
        return insttoks + 1;  // pray that you resync later.
    } // if

    ctx->coissue = coissue;
    if (coissue)
    {
        if (!shader_is_pixel(ctx))
            fail(ctx, "coissue instruction on non-pixel shader");
        if (shader_version_atleast(ctx, 2, 0))
            fail(ctx, "coissue instruction in Shader Model >= 2.0");
    } // if

    if ((ctx->shader_type & instruction->shader_types) == 0)
    {
        failf(ctx, "opcode '%s' not available in this shader type.",
                instruction->opcode_string);
    } // if

    memset(ctx->dwords, '\0', sizeof (ctx->dwords));
    ctx->instruction_controls = controls;
    ctx->predicated = predicated;

    // Update the context with instruction's arguments.
    adjust_token_position(ctx, 1);
    retval = instruction->parse_args(ctx);

    if (predicated)
        retval += parse_predicated_token(ctx);

    // parse_args() moves these forward for convenience...reset them.
    ctx->tokens = start_tokens;
    ctx->tokencount = start_tokencount;
    ctx->current_position = start_position;

    if (instruction->state != NULL)
        instruction->state(ctx);

    ctx->instruction_count += instruction->slots;

    if (!isfail(ctx))
        emitter(ctx);  // call the profile's emitter.

    if (ctx->reset_texmpad)
    {
        ctx->texm3x2pad_dst0 = -1;
        ctx->texm3x2pad_src0 = -1;
        ctx->texm3x3pad_dst0 = -1;
        ctx->texm3x3pad_src0 = -1;
        ctx->texm3x3pad_dst1 = -1;
        ctx->texm3x3pad_src1 = -1;
        ctx->reset_texmpad = 0;
    } // if

    ctx->previous_opcode = opcode;
    ctx->scratch_registers = 0;  // reset after every instruction.

    if (!shader_version_atleast(ctx, 2, 0))
    {
        if (insttoks != 0)  // reserved field in shaders < 2.0 ...
            fail(ctx, "instruction token count must be zero");
    } // if
    else
    {
        if (((uint32)retval) != (insttoks+1))
        {
            failf(ctx, "wrong token count (%u, not %u) for opcode '%s'.",
                    (uint) retval, (uint) (insttoks+1),
                    instruction->opcode_string);
            retval = insttoks + 1;  // try to keep sync.
        } // if
    } // else

    return retval;
} // parse_instruction_token


static int parse_version_token(Context *ctx, const char *profilestr)
{
    if (ctx->tokencount == 0)
    {
        fail(ctx, "Expected version token, got none at all.");
        return 0;
    } // if

    const uint32 token = SWAP32(*(ctx->tokens));
    const uint32 shadertype = ((token >> 16) & 0xFFFF);
    const uint8 major = (uint8) ((token >> 8) & 0xFF);
    const uint8 minor = (uint8) (token & 0xFF);

    ctx->version_token = token;

    // 0xFFFF == pixel shader, 0xFFFE == vertex shader
    if (shadertype == 0xFFFF)
    {
        ctx->shader_type = MOJOSHADER_TYPE_PIXEL;
        ctx->shader_type_str = "ps";
    } // if
    else if (shadertype == 0xFFFE)
    {
        ctx->shader_type = MOJOSHADER_TYPE_VERTEX;
        ctx->shader_type_str = "vs";
    } // else if
    else  // geometry shader? Bogus data?
    {
        fail(ctx, "Unsupported shader type or not a shader at all");
        return -1;
    } // else

    ctx->major_ver = major;
    ctx->minor_ver = minor;

    if (!shader_version_supported(major, minor))
    {
        failf(ctx, "Shader Model %u.%u is currently unsupported.",
                (uint) major, (uint) minor);
    } // if

    if (!isfail(ctx))
        ctx->profile->start_emitter(ctx, profilestr);

    return 1;  // ate one token.
} // parse_version_token


static int parse_ctab_string(const uint8 *start, const uint32 bytes,
                             const uint32 name)
{
    // Make sure strings don't overflow the CTAB buffer...
    if (name < bytes)
    {
        int i;
        const int slenmax = bytes - name;
        const char *namestr = (const char *) (start + name);
        for (i = 0; i < slenmax; i++)
        {
            if (namestr[i] == '\0')
                return 1;  // it's okay.
        } // for
    } // if

    return 0;  // overflowed.
} // parse_ctab_string


static int parse_ctab_typeinfo(Context *ctx, const uint8 *start,
                               const uint32 bytes, const uint32 pos,
                               MOJOSHADER_symbolTypeInfo *info,
                               const int depth)
{
    if ((bytes <= pos) || ((bytes - pos) < 16))
        return 0;  // corrupt CTAB.

    const uint16 *typeptr = (const uint16 *) (start + pos);

    info->parameter_class = (MOJOSHADER_symbolClass) SWAP16(typeptr[0]);
    info->parameter_type = (MOJOSHADER_symbolType) SWAP16(typeptr[1]);
    info->rows = (unsigned int) SWAP16(typeptr[2]);
    info->columns = (unsigned int) SWAP16(typeptr[3]);
    info->elements = (unsigned int) SWAP16(typeptr[4]);

    if (info->parameter_class >= MOJOSHADER_SYMCLASS_TOTAL)
    {
        failf(ctx, "Unknown parameter class (0x%X)", info->parameter_class);
        info->parameter_class = MOJOSHADER_SYMCLASS_SCALAR;
    } // if

    if (info->parameter_type >= MOJOSHADER_SYMTYPE_TOTAL)
    {
        failf(ctx, "Unknown parameter type (0x%X)", info->parameter_type);
        info->parameter_type = MOJOSHADER_SYMTYPE_INT;
    } // if

    const unsigned int member_count = (unsigned int) SWAP16(typeptr[5]);
    info->member_count = 0;
    info->members = NULL;

    if ((pos + 16 + (member_count * 8)) >= bytes)
        return 0;  // corrupt CTAB.

    if (member_count > 0)
    {
        if (depth > 300)  // make sure we aren't in an infinite loop here.
        {
            fail(ctx, "Possible infinite loop in CTAB structure.");
            return 0;
        } // if

        const size_t len = sizeof (MOJOSHADER_symbolStructMember) * member_count;
        info->members = (MOJOSHADER_symbolStructMember *) Malloc(ctx, len);
        if (info->members == NULL)
            return 1;  // we'll check ctx->out_of_memory later.
        memset(info->members, '\0', len);
        info->member_count = member_count;
    } // else

    unsigned int i;
    const uint32 *member = (const uint32 *) (start + typeptr[6]);
    for (i = 0; i < member_count; i++)
    {
        MOJOSHADER_symbolStructMember *mbr = &info->members[i];
        const uint32 name = SWAP32(member[0]);
        const uint32 memberinfopos = SWAP32(member[1]);
        member += 2;

        if (!parse_ctab_string(start, bytes, name))
            return 0;  // info->members will be free()'d elsewhere.

        mbr->name = StrDup(ctx, (const char *) (start + name));
        if (mbr->name == NULL)
            return 1;  // we'll check ctx->out_of_memory later.
        if (!parse_ctab_typeinfo(ctx, start, bytes, memberinfopos, &mbr->info, depth + 1))
            return 0;
        if (ctx->out_of_memory)
            return 1;  // drop out now.
    } // for

    return 1;
} // parse_ctab_typeinfo


// Microsoft's tools add a CTAB comment to all shaders. This is the
//  "constant table," or specifically: D3DXSHADER_CONSTANTTABLE:
//  http://msdn.microsoft.com/en-us/library/bb205440(VS.85).aspx
// This may tell us high-level truths about an otherwise generic low-level
//  registers, for instance, how large an array actually is, etc.
static void parse_constant_table(Context *ctx, const uint32 *tokens,
                                 const uint32 bytes, const uint32 okay_version,
                                 const int setvariables, CtabData *ctab)
{
    const uint32 id = SWAP32(tokens[1]);
    if (id != CTAB_ID)
        return;  // not the constant table.

    if (ctab->have_ctab)  // !!! FIXME: can you have more than one?
    {
        fail(ctx, "Shader has multiple CTAB sections");
        return;
    } // if

    ctab->have_ctab = 1;

    const uint8 *start = (uint8 *) &tokens[2];

    if (bytes < 32)
    {
        fail(ctx, "Truncated CTAB data");
        return;
    } // if

    const uint32 size = SWAP32(tokens[2]);
    const uint32 creator = SWAP32(tokens[3]);
    const uint32 version = SWAP32(tokens[4]);
    const uint32 constants = SWAP32(tokens[5]);
    const uint32 constantinfo = SWAP32(tokens[6]);
    const uint32 target = SWAP32(tokens[8]);

    if (size != CTAB_SIZE)
        goto corrupt_ctab;
    else if (constants > 1000000)  // sanity check.
        goto corrupt_ctab;

    if (version != okay_version) goto corrupt_ctab;
    if (creator >= bytes) goto corrupt_ctab;
    if (constantinfo >= bytes) goto corrupt_ctab;
    if ((bytes - constantinfo) < (constants * CINFO_SIZE)) goto corrupt_ctab;
    if (target >= bytes) goto corrupt_ctab;
    if (!parse_ctab_string(start, bytes, target)) goto corrupt_ctab;
    // !!! FIXME: check that (start+target) points to "ps_3_0", etc.

    ctab->symbols = NULL;
    if (constants > 0)
    {
        ctab->symbols = (MOJOSHADER_symbol *) Malloc(ctx, sizeof (MOJOSHADER_symbol) * constants);
        if (ctab->symbols == NULL)
            return;
        memset(ctab->symbols, '\0', sizeof (MOJOSHADER_symbol) * constants);
    } // if
    ctab->symbol_count = constants;

    uint32 i = 0;
    for (i = 0; i < constants; i++)
    {
        const uint8 *ptr = start + constantinfo + (i * CINFO_SIZE);
        const uint32 name = SWAP32(*((uint32 *) (ptr + 0)));
        const uint16 regset = SWAP16(*((uint16 *) (ptr + 4)));
        const uint16 regidx = SWAP16(*((uint16 *) (ptr + 6)));
        const uint16 regcnt = SWAP16(*((uint16 *) (ptr + 8)));
        const uint32 typeinf = SWAP32(*((uint32 *) (ptr + 12)));
        const uint32 defval = SWAP32(*((uint32 *) (ptr + 16)));
        MOJOSHADER_uniformType mojotype = MOJOSHADER_UNIFORM_UNKNOWN;

        if (!parse_ctab_string(start, bytes, name)) goto corrupt_ctab;
        if (defval >= bytes) goto corrupt_ctab;

        switch (regset)
        {
            case 0: mojotype = MOJOSHADER_UNIFORM_BOOL; break;
            case 1: mojotype = MOJOSHADER_UNIFORM_INT; break;
            case 2: mojotype = MOJOSHADER_UNIFORM_FLOAT; break;
            case 3: /* SAMPLER */ break;
            default: goto corrupt_ctab;
        } // switch

        if ((setvariables) && (mojotype != MOJOSHADER_UNIFORM_UNKNOWN))
        {
            VariableList *item;
            item = (VariableList *) Malloc(ctx, sizeof (VariableList));
            if (item != NULL)
            {
                item->type = mojotype;
                item->index = regidx;
                item->count = regcnt;
                item->constant = NULL;
                item->used = 0;
                item->emit_position = -1;
                item->next = ctx->variables;
                ctx->variables = item;
            } // if
        } // if

        // Add the symbol.
        const char *namecpy = StrDup(ctx, (const char *) (start + name));
        if (namecpy == NULL)
            return;

        MOJOSHADER_symbol *sym = &ctab->symbols[i];
        sym->name = namecpy;
        sym->register_set = (MOJOSHADER_symbolRegisterSet) regset;
        sym->register_index = (unsigned int) regidx;
        sym->register_count = (unsigned int) regcnt;
        if (!parse_ctab_typeinfo(ctx, start, bytes, typeinf, &sym->info, 0))
            goto corrupt_ctab;  // sym->name will get free()'d later.
        else if (ctx->out_of_memory)
            return;  // just bail now.
    } // for

    return;

corrupt_ctab:
    fail(ctx, "Shader has corrupt CTAB data");
} // parse_constant_table


static void free_symbols(MOJOSHADER_free f, void *d, MOJOSHADER_symbol *syms,
                         const int symcount);


static int is_comment_token(Context *ctx, const uint32 tok, uint32 *tokcount)
{
    const uint32 token = SWAP32(tok);
    if ((token & 0xFFFF) == 0xFFFE)  // actually a comment token?
    {
        if ((token & 0x80000000) != 0)
            fail(ctx, "comment token high bit must be zero.");  // so says msdn.
        *tokcount = ((token >> 16) & 0xFFFF);
        return 1;
    } // if

    return 0;
} // is_comment_token


typedef struct PreshaderBlockInfo
{
    const uint32 *tokens;
    uint32 tokcount;
    int seen;
} PreshaderBlockInfo;

// Preshaders only show up in compiled Effect files. The format is
//  undocumented, and even the instructions aren't the same opcodes as you
//  would find in a regular shader. These things show up because the HLSL
//  compiler can detect work that sets up constant registers that could
//  be moved out of the shader itself. Preshaders run once, then the shader
//  itself runs many times, using the constant registers the preshader has set
//  up. There are cases where the preshaders are 3+ times as many instructions
//  as the shader itself, so this can be a big performance win.
// My presumption is that Microsoft's Effects framework runs the preshaders on
//  the CPU, then loads the constant register file appropriately before handing
//  off to the GPU. As such, we do the same.
static void parse_preshader(Context *ctx, const uint32 *tokens, uint32 tokcount)
{
#ifndef MOJOSHADER_EFFECT_SUPPORT
    fail(ctx, "Preshader found, but effect support is disabled!");
#else
    uint32 i;

    assert(ctx->have_preshader == 0);  // !!! FIXME: can you have more than one?
    ctx->have_preshader = 1;

    // !!! FIXME: I don't know what specific versions signify, but we need to
    // !!! FIXME:  save this to test against the CTAB version field, if
    // !!! FIXME:  nothing else.
    // !!! FIXME: 0x02 0x0? is probably the version (fx_2_?),
    // !!! FIXME:  and 0x4658 is the magic, like a real shader's version token.
    const uint32 version_magic = 0x46580000;
    const uint32 min_version = 0x00000200 | version_magic;
    const uint32 max_version = 0x00000201 | version_magic;
    const uint32 version = SWAP32(tokens[0]);
    if (version < min_version || version > max_version)
    {
        fail(ctx, "Unsupported preshader version.");
        return;  // fail because the shader will malfunction w/o this.
    } // if

    tokens++;
    tokcount--;

    // All sections of a preshader are packed into separate comment tokens,
    //  inside the containing comment token block. Find them all before
    //  we start, so we don't care about the order they appear in the file.
    PreshaderBlockInfo ctab = { 0, 0, 0 };
    PreshaderBlockInfo prsi = { 0, 0, 0 };
    PreshaderBlockInfo fxlc = { 0, 0, 0 };
    PreshaderBlockInfo clit = { 0, 0, 0 };

    while (tokcount > 0)
    {
        uint32 subtokcount = 0;
        if ( (!is_comment_token(ctx, *tokens, &subtokcount)) ||
             (subtokcount > tokcount) )
        {
            // !!! FIXME: Standalone preshaders have this EOS-looking token,
            // !!! FIXME:  sometimes followed by tokens that don't appear to
            // !!! FIXME:  have anything to do with the rest of the blob.
            // !!! FIXME: So for now, treat this as a special "EOS" comment.
            if (SWAP32(*tokens) == 0xFFFF)
                break;

            fail(ctx, "Bogus preshader data.");
            return;
        } // if

        tokens++;
        tokcount--;

        const uint32 *nexttokens = tokens + subtokcount;
        const uint32 nexttokcount = tokcount - subtokcount;

        if (subtokcount > 0)
        {
            switch (SWAP32(*tokens))
            {
                #define PRESHADER_BLOCK_CASE(id, var) \
                    case id##_ID: { \
                        if (var.seen) { \
                            fail(ctx, "Multiple " #id " preshader blocks."); \
                            return; \
                        } \
                        var.tokens = tokens; \
                        var.tokcount = subtokcount; \
                        var.seen = 1; \
                        break; \
                    }
                PRESHADER_BLOCK_CASE(CTAB, ctab);
                PRESHADER_BLOCK_CASE(PRSI, prsi);
                PRESHADER_BLOCK_CASE(FXLC, fxlc);
                PRESHADER_BLOCK_CASE(CLIT, clit);
                default: fail(ctx, "Bogus preshader section."); return;
                #undef PRESHADER_BLOCK_CASE
            } // switch
        } // if

        tokens = nexttokens;
        tokcount = nexttokcount;
    } // while

    if (!ctab.seen) { fail(ctx, "No CTAB block in preshader."); return; }
    if (!fxlc.seen) { fail(ctx, "No FXLC block in preshader."); return; }
    if (!clit.seen) { fail(ctx, "No CLIT block in preshader."); return; }
    // prsi.seen is optional, apparently.

    MOJOSHADER_preshader *preshader = (MOJOSHADER_preshader *)
                                    Malloc(ctx, sizeof (MOJOSHADER_preshader));
    if (preshader == NULL)
        return;

    memset(preshader, '\0', sizeof (MOJOSHADER_preshader));
    preshader->malloc = ctx->malloc;
    preshader->free = ctx->free;
    preshader->malloc_data = ctx->malloc_data;

    ctx->preshader = preshader;

    // Let's set up the constant literals first...
    if (clit.tokcount == 0)
        fail(ctx, "Bogus CLIT block in preshader.");
    else
    {
        const uint32 lit_count = SWAP32(clit.tokens[1]);
        if (lit_count > ((clit.tokcount - 2) / 2))
        {
            fail(ctx, "Bogus CLIT block in preshader.");
            return;
        } // if
        else if (lit_count > 0)
        {
            preshader->literal_count = (unsigned int) lit_count;
            assert(sizeof (double) == 8);  // just in case.
            const size_t len = sizeof (double) * lit_count;
            preshader->literals = (double *) Malloc(ctx, len);
            if (preshader->literals == NULL)
                return;  // oh well.
            const double *litptr = (const double *) (clit.tokens + 2);
            for (i = 0; i < lit_count; i++)
                preshader->literals[i] = SWAPDBL(litptr[i]);
        } // else if
    } // else

    // Parse out the PRSI block. This is used to map the output registers.
    uint32 output_map_count = 0;
    const uint32 *output_map = NULL;
    if (prsi.seen)
    {
        if (prsi.tokcount < 8)
        {
            fail(ctx, "Bogus preshader PRSI data");
            return;
        } // if

        //const uint32 first_output_reg = SWAP32(prsi.tokens[1]);
        // !!! FIXME: there are a lot of fields here I don't know about.
        // !!! FIXME:  maybe [2] and [3] are for int4 and bool registers?
        //const uint32 output_reg_count = SWAP32(prsi.tokens[4]);
        // !!! FIXME:  maybe [5] and [6] are for int4 and bool registers?
        output_map_count = SWAP32(prsi.tokens[7]);

        prsi.tokcount -= 8;
        prsi.tokens += 8;

        if (prsi.tokcount < ((output_map_count + 1) * 2))
        {
            fail(ctx, "Bogus preshader PRSI data");
            return;
        } // if

        output_map = prsi.tokens;
    } // if

    // Now we'll figure out the CTAB...
    CtabData ctabdata = { 0, 0, 0 };
    parse_constant_table(ctx, ctab.tokens - 1, ctab.tokcount * 4,
                         version, 0, &ctabdata);

    // preshader owns this now. Don't free it in this function.
    preshader->symbol_count = ctabdata.symbol_count;
    preshader->symbols = ctabdata.symbols;

    if (!ctabdata.have_ctab)
    {
        fail(ctx, "Bogus preshader CTAB data");
        return;
    } // if

    // The FXLC block has the actual instructions...
    uint32 opcode_count = SWAP32(fxlc.tokens[1]);

    const size_t len = sizeof (MOJOSHADER_preshaderInstruction) * opcode_count;
    preshader->instruction_count = (unsigned int) opcode_count;
    preshader->instructions = (MOJOSHADER_preshaderInstruction *) Malloc(ctx, len);
    if (preshader->instructions == NULL)
        return;
    memset(preshader->instructions, '\0', len);

    fxlc.tokens += 2;
    fxlc.tokcount -= 2;
    if (opcode_count > (fxlc.tokcount / 2))
    {
        fail(ctx, "Bogus preshader FXLC block.");
        return;
    } // if

    MOJOSHADER_preshaderInstruction *inst = preshader->instructions;
    while (opcode_count--)
    {
        const uint32 opcodetok = SWAP32(fxlc.tokens[0]);
        MOJOSHADER_preshaderOpcode opcode = MOJOSHADER_PRESHADEROP_NOP;
        switch ((opcodetok >> 16) & 0xFFFF)
        {
            case 0x1000: opcode = MOJOSHADER_PRESHADEROP_MOV; break;
            case 0x1010: opcode = MOJOSHADER_PRESHADEROP_NEG; break;
            case 0x1030: opcode = MOJOSHADER_PRESHADEROP_RCP; break;
            case 0x1040: opcode = MOJOSHADER_PRESHADEROP_FRC; break;
            case 0x1050: opcode = MOJOSHADER_PRESHADEROP_EXP; break;
            case 0x1060: opcode = MOJOSHADER_PRESHADEROP_LOG; break;
            case 0x1070: opcode = MOJOSHADER_PRESHADEROP_RSQ; break;
            case 0x1080: opcode = MOJOSHADER_PRESHADEROP_SIN; break;
            case 0x1090: opcode = MOJOSHADER_PRESHADEROP_COS; break;
            case 0x10A0: opcode = MOJOSHADER_PRESHADEROP_ASIN; break;
            case 0x10B0: opcode = MOJOSHADER_PRESHADEROP_ACOS; break;
            case 0x10C0: opcode = MOJOSHADER_PRESHADEROP_ATAN; break;
            case 0x2000: opcode = MOJOSHADER_PRESHADEROP_MIN; break;
            case 0x2010: opcode = MOJOSHADER_PRESHADEROP_MAX; break;
            case 0x2020: opcode = MOJOSHADER_PRESHADEROP_LT; break;
            case 0x2030: opcode = MOJOSHADER_PRESHADEROP_GE; break;
            case 0x2040: opcode = MOJOSHADER_PRESHADEROP_ADD; break;
            case 0x2050: opcode = MOJOSHADER_PRESHADEROP_MUL; break;
            case 0x2060: opcode = MOJOSHADER_PRESHADEROP_ATAN2; break;
            case 0x2080: opcode = MOJOSHADER_PRESHADEROP_DIV; break;
            case 0x3000: opcode = MOJOSHADER_PRESHADEROP_CMP; break;
            case 0x3010: opcode = MOJOSHADER_PRESHADEROP_MOVC; break;
            case 0x5000: opcode = MOJOSHADER_PRESHADEROP_DOT; break;
            case 0x5020: opcode = MOJOSHADER_PRESHADEROP_NOISE; break;
            case 0xA000: opcode = MOJOSHADER_PRESHADEROP_MIN_SCALAR; break;
            case 0xA010: opcode = MOJOSHADER_PRESHADEROP_MAX_SCALAR; break;
            case 0xA020: opcode = MOJOSHADER_PRESHADEROP_LT_SCALAR; break;
            case 0xA030: opcode = MOJOSHADER_PRESHADEROP_GE_SCALAR; break;
            case 0xA040: opcode = MOJOSHADER_PRESHADEROP_ADD_SCALAR; break;
            case 0xA050: opcode = MOJOSHADER_PRESHADEROP_MUL_SCALAR; break;
            case 0xA060: opcode = MOJOSHADER_PRESHADEROP_ATAN2_SCALAR; break;
            case 0xA080: opcode = MOJOSHADER_PRESHADEROP_DIV_SCALAR; break;
            case 0xD000: opcode = MOJOSHADER_PRESHADEROP_DOT_SCALAR; break;
            case 0xD020: opcode = MOJOSHADER_PRESHADEROP_NOISE_SCALAR; break;
            default: fail(ctx, "Unknown preshader opcode."); break;
        } // switch

        uint32 operand_count = SWAP32(fxlc.tokens[1]) + 1;  // +1 for dest.

        inst->opcode = opcode;
        inst->element_count = (unsigned int) (opcodetok & 0xFF);
        inst->operand_count = (unsigned int) operand_count;

        fxlc.tokens += 2;
        fxlc.tokcount -= 2;
        if ((operand_count * 3) > fxlc.tokcount)
        {
            fail(ctx, "Bogus preshader FXLC block.");
            return;
        } // if

        MOJOSHADER_preshaderOperand *operand = inst->operands;
        while (operand_count--)
        {
            const unsigned int item = (unsigned int) SWAP32(fxlc.tokens[2]);

            // !!! FIXME: Is this used anywhere other than INPUT? -flibit
            const uint32 numarrays = SWAP32(fxlc.tokens[0]);
            switch (SWAP32(fxlc.tokens[1]))
            {
                case 1:  // literal from CLIT block.
                {
                    if (item > preshader->literal_count)
                    {
                        fail(ctx, "Bogus preshader literal index.");
                        break;
                    } // if
                    operand->type = MOJOSHADER_PRESHADEROPERAND_LITERAL;
                    break;
                } // case

                case 2:  // item from ctabdata.
                {
                    MOJOSHADER_symbol *sym = ctabdata.symbols;
                    const uint32 symcount = (uint32) ctabdata.symbol_count;
                    for (i = 0; i < symcount; i++, sym++)
                    {
                        const uint32 base = sym->register_index * 4;
                        const uint32 count = sym->register_count * 4;
                        assert(sym->register_set==MOJOSHADER_SYMREGSET_FLOAT4);
                        if ( (base <= item) && ((base + count) > item) )
                            break;
                    } // for
                    if (i == ctabdata.symbol_count)
                    {
                        fail(ctx, "Bogus preshader input index.");
                        break;
                    } // if
                    operand->type = MOJOSHADER_PRESHADEROPERAND_INPUT;
                    if (numarrays > 0)
                    {
                        // malloc the array symbol name array
                        const uint32 siz = numarrays * sizeof (uint32);
                        operand->array_register_count = numarrays;
                        operand->array_registers = (uint32 *) Malloc(ctx, siz);
                        memset(operand->array_registers, '\0', siz);
                        // Get each register base, indicating the arrays used.
                        // !!! FIXME: fail if fxlc.tokcount*2 > numarrays ?
                        for (i = 0; i < numarrays; i++)
                        {
                            const uint32 jmp = SWAP32(fxlc.tokens[4]);
                            const uint32 bigjmp = (jmp >> 4) * 4;
                            const uint32 ltljmp = (jmp >> 2) & 3;
                            operand->array_registers[i] = bigjmp + ltljmp;
                            fxlc.tokens += 2;
                            fxlc.tokcount -= 2;
                        } // for
                    } // if
                    break;
                } // case

                case 4:
                {
                    operand->type = MOJOSHADER_PRESHADEROPERAND_OUTPUT;

                    for (i = 0; i < output_map_count; i++)
                    {
                        const uint32 base = output_map[(i*2)] * 4;
                        const uint32 count = output_map[(i*2)+1] * 4;
                        if ( (base <= item) && ((base + count) > item) )
                            break;
                    } // for

                    if (i == output_map_count)
                    {
                        if (prsi.seen)  // No PRSI tokens, no output map.
                            fail(ctx, "Bogus preshader output index.");
                    } // if

                    break;
                } // case

                case 7:
                {
                    operand->type = MOJOSHADER_PRESHADEROPERAND_TEMP;
                    if (item >= preshader->temp_count)
                        preshader->temp_count = item + 1;
                    break;
                } // case
            } // switch

            operand->index = item;

            fxlc.tokens += 3;
            fxlc.tokcount -= 3;
            operand++;
        } // while

        inst++;
    } // while

    // Registers need to be vec4, round up to nearest 4
    preshader->temp_count = (preshader->temp_count + 3) & ~3;

    unsigned int largest = 0;
    const MOJOSHADER_symbol *sym = preshader->symbols;
    const uint32 symcount = (uint32) preshader->symbol_count;
    for (i = 0; i < symcount; i++, sym++)
    {
        const unsigned int val = sym->register_index + sym->register_count;
        if (val > largest)
            largest = val;
    } // for

    if (largest > 0)
    {
        const size_t len = largest * sizeof (float) * 4;
        preshader->registers = (float *) Malloc(ctx, len);
        memset(preshader->registers, '\0', len);
        preshader->register_count = largest;
    } // if
#endif
} // parse_preshader

static int parse_comment_token(Context *ctx)
{
    uint32 commenttoks = 0;
    if (is_comment_token(ctx, *ctx->tokens, &commenttoks))
    {
        if ((commenttoks >= 2) && (commenttoks < ctx->tokencount))
        {
            const uint32 id = SWAP32(ctx->tokens[1]);
            if (id == PRES_ID)
                parse_preshader(ctx, ctx->tokens + 2, commenttoks - 2);
            else if (id == CTAB_ID)
            {
                parse_constant_table(ctx, ctx->tokens, commenttoks * 4,
                                     ctx->version_token, 1, &ctx->ctab);
            } // else if
        } // if
        return commenttoks + 1;  // comment data plus the initial token.
    } // if

    return 0;  // not a comment token.
} // parse_comment_token


static int parse_end_token(Context *ctx)
{
    if (SWAP32(*(ctx->tokens)) != 0x0000FFFF)   // end token always 0x0000FFFF.
        return 0;  // not us, eat no tokens.

    if (!ctx->know_shader_size)  // this is the end of stream!
        ctx->tokencount = 1;
    else if (ctx->tokencount != 1)  // we _must_ be last. If not: fail.
        fail(ctx, "end token before end of stream");

    if (!isfail(ctx))
        ctx->profile->end_emitter(ctx);

    return 1;
} // parse_end_token


static int parse_phase_token(Context *ctx)
{
    // !!! FIXME: needs state; allow only one phase token per shader, I think?
    if (SWAP32(*(ctx->tokens)) != 0x0000FFFD) // phase token always 0x0000FFFD.
        return 0;  // not us, eat no tokens.

    if ( (!shader_is_pixel(ctx)) || (!shader_version_exactly(ctx, 1, 4)) )
        fail(ctx, "phase token only available in 1.4 pixel shaders");

    if (!isfail(ctx))
        ctx->profile->phase_emitter(ctx);

    return 1;
} // parse_phase_token


static int parse_token(Context *ctx)
{
    int rc = 0;

    assert(ctx->output_stack_len == 0);

    if (ctx->tokencount == 0)
        fail(ctx, "unexpected end of shader.");

    else if ((rc = parse_comment_token(ctx)) != 0)
        return rc;

    else if ((rc = parse_end_token(ctx)) != 0)
        return rc;

    else if ((rc = parse_phase_token(ctx)) != 0)
        return rc;

    else if ((rc = parse_instruction_token(ctx)) != 0)
        return rc;

    failf(ctx, "unknown token (0x%x)", (uint) *ctx->tokens);
    return 1;  // good luck!
} // parse_token


static int find_profile_id(const char *profile)
{
    size_t i;
    for (i = 0; i < STATICARRAYLEN(profileMap); i++)
    {
        const char *name = profileMap[i].from;
        if (strcmp(name, profile) == 0)
        {
            profile = profileMap[i].to;
            break;
        } // if
    } // for

    for (i = 0; i < STATICARRAYLEN(profiles); i++)
    {
        const char *name = profiles[i].name;
        if (strcmp(name, profile) == 0)
            return i;
    } // for

    return -1;  // no match.
} // find_profile_id


static Context *build_context(const char *profile,
                              const char *mainfn,
                              const unsigned char *tokenbuf,
                              const unsigned int bufsize,
                              const MOJOSHADER_swizzle *swiz,
                              const unsigned int swizcount,
                              const MOJOSHADER_samplerMap *smap,
                              const unsigned int smapcount,
                              MOJOSHADER_malloc m, MOJOSHADER_free f, void *d)
{
    if (m == NULL) m = MOJOSHADER_internal_malloc;
    if (f == NULL) f = MOJOSHADER_internal_free;

    Context *ctx = (Context *) m(sizeof (Context), d);
    if (ctx == NULL)
        return NULL;

    memset(ctx, '\0', sizeof (Context));
    ctx->malloc = m;
    ctx->free = f;
    ctx->malloc_data = d;
    ctx->tokens = (const uint32 *) tokenbuf;
    ctx->orig_tokens = (const uint32 *) tokenbuf;
    ctx->know_shader_size = (bufsize != 0);
    ctx->tokencount = ctx->know_shader_size ? (bufsize / sizeof (uint32)) : 0xFFFFFFFF;
    ctx->swizzles = swiz;
    ctx->swizzles_count = swizcount;
    ctx->samplermap = smap;
    ctx->samplermap_count = smapcount;
    ctx->endline = ENDLINE_STR;
    ctx->endline_len = strlen(ctx->endline);
    ctx->last_address_reg_component = -1;
    ctx->current_position = MOJOSHADER_POSITION_BEFORE;
    ctx->texm3x2pad_dst0 = -1;
    ctx->texm3x2pad_src0 = -1;
    ctx->texm3x3pad_dst0 = -1;
    ctx->texm3x3pad_src0 = -1;
    ctx->texm3x3pad_dst1 = -1;
    ctx->texm3x3pad_src1 = -1;

    ctx->errors = errorlist_create(MallocBridge, FreeBridge, ctx);
    if (ctx->errors == NULL)
    {
        f(ctx, d);
        return NULL;
    } // if

    if (!set_output(ctx, &ctx->mainline))
    {
        errorlist_destroy(ctx->errors);
        f(ctx, d);
        return NULL;
    } // if

    if (mainfn != NULL)
    {
        if (strlen(mainfn) > 55)  // !!! FIXME: just to keep things sane. Lots of hardcoded stack arrays...
            failf(ctx, "Main function name '%s' is too big", mainfn);
        else
            ctx->mainfn = StrDup(ctx, mainfn);
    } // if

    if (profile != NULL)
    {
        const int profileid = find_profile_id(profile);
        ctx->profileid = profileid;
        if (profileid >= 0)
            ctx->profile = &profiles[profileid];
        else
            failf(ctx, "Profile '%s' is unknown or unsupported", profile);
    } // if

    return ctx;
} // build_context


static void free_constants_list(MOJOSHADER_free f, void *d, ConstantsList *item)
{
    while (item != NULL)
    {
        ConstantsList *next = item->next;
        f(item, d);
        item = next;
    } // while
} // free_constants_list


static void free_variable_list(MOJOSHADER_free f, void *d, VariableList *item)
{
    while (item != NULL)
    {
        VariableList *next = item->next;
        f(item, d);
        item = next;
    } // while
} // free_variable_list


static void free_sym_typeinfo(MOJOSHADER_free f, void *d,
                              MOJOSHADER_symbolTypeInfo *typeinfo)
{
    unsigned int i;
    for (i = 0; i < typeinfo->member_count; i++)
    {
        f((void *) typeinfo->members[i].name, d);
        free_sym_typeinfo(f, d, &typeinfo->members[i].info);
    } // for
    f((void *) typeinfo->members, d);
} // free_sym_members


static void free_symbols(MOJOSHADER_free f, void *d, MOJOSHADER_symbol *syms,
                         const int symcount)
{
    int i;
    for (i = 0; i < symcount; i++)
    {
        f((void *) syms[i].name, d);
        free_sym_typeinfo(f, d, &syms[i].info);
    } // for
    f((void *) syms, d);
} // free_symbols


static void destroy_context(Context *ctx)
{
    if (ctx != NULL)
    {
        MOJOSHADER_free f = ((ctx->free != NULL) ? ctx->free : MOJOSHADER_internal_free);
        void *d = ctx->malloc_data;
        buffer_destroy(ctx->preflight);
        buffer_destroy(ctx->globals);
        buffer_destroy(ctx->inputs);
        buffer_destroy(ctx->outputs);
        buffer_destroy(ctx->helpers);
        buffer_destroy(ctx->subroutines);
        buffer_destroy(ctx->mainline_intro);
        buffer_destroy(ctx->mainline_arguments);
        buffer_destroy(ctx->mainline_top);
        buffer_destroy(ctx->mainline);
        buffer_destroy(ctx->postflight);
        buffer_destroy(ctx->ignore);
        free_constants_list(f, d, ctx->constants);
        free_reglist(f, d, ctx->used_registers.next);
        free_reglist(f, d, ctx->defined_registers.next);
        free_reglist(f, d, ctx->uniforms.next);
        free_reglist(f, d, ctx->attributes.next);
        free_reglist(f, d, ctx->samplers.next);
        free_variable_list(f, d, ctx->variables);
        errorlist_destroy(ctx->errors);
        free_symbols(f, d, ctx->ctab.symbols, ctx->ctab.symbol_count);
        MOJOSHADER_freePreshader(ctx->preshader);
        f((void *) ctx->mainfn, d);
        f(ctx, d);
    } // if
} // destroy_context


static char *build_output(Context *ctx, size_t *len)
{
    // add a byte for a null terminator.
    Buffer *buffers[] = {
        ctx->preflight, ctx->globals, ctx->inputs, ctx->outputs, ctx->helpers,
        ctx->subroutines, ctx->mainline_intro, ctx->mainline_arguments,
        ctx->mainline_top, ctx->mainline, ctx->postflight
        // don't append ctx->ignore ... that's why it's called "ignore"
    };
    char *retval = buffer_merge(buffers, STATICARRAYLEN(buffers), len);
    return retval;
} // build_output


static inline const char *alloc_varname(Context *ctx, const RegisterList *reg)
{
    return ctx->profile->get_varname(ctx, reg->regtype, reg->regnum);
} // alloc_varname


// !!! FIXME: this code is sort of hard to follow:
// !!! FIXME:  "var->used" only applies to arrays (at the moment, at least,
// !!! FIXME:  but this might be buggy at a later time?), and this code
// !!! FIXME:  relies on that.
// !!! FIXME: "variables" means "things we found in a CTAB" but it's not
// !!! FIXME:  all registers, etc.
// !!! FIXME: "const_array" means an array for d3d "const" registers (c0, c1,
// !!! FIXME:  etc), but not a constant array, although they _can_ be.
// !!! FIXME: It's just a mess.  :/
static MOJOSHADER_uniform *build_uniforms(Context *ctx)
{
    const size_t len = sizeof (MOJOSHADER_uniform) * ctx->uniform_count;
    MOJOSHADER_uniform *retval = (MOJOSHADER_uniform *) Malloc(ctx, len);

    if (retval != NULL)
    {
        MOJOSHADER_uniform *wptr = retval;
        memset(wptr, '\0', len);

        VariableList *var;
        int written = 0;
        for (var = ctx->variables; var != NULL; var = var->next)
        {
            if (var->used)
            {
                const char *name = ctx->profile->get_const_array_varname(ctx,
                                                      var->index, var->count);
                if (name != NULL)
                {
                    wptr->type = MOJOSHADER_UNIFORM_FLOAT;
                    wptr->index = var->index;
                    wptr->array_count = var->count;
                    wptr->constant = (var->constant != NULL) ? 1 : 0;
                    wptr->name = name;
                    wptr++;
                    written++;
                } // if
            } // if
        } // for

        RegisterList *item = ctx->uniforms.next;
        MOJOSHADER_uniformType type = MOJOSHADER_UNIFORM_FLOAT;
        while (written < ctx->uniform_count)
        {
            int skip = 0;

            // !!! FIXME: does this fail if written > ctx->uniform_count?
            if (item == NULL)
            {
                fail(ctx, "BUG: mismatched uniform list and count");
                break;
            } // if

            int index = item->regnum;
            switch (item->regtype)
            {
                case REG_TYPE_CONST:
                    skip = (item->array != NULL);
                    type = MOJOSHADER_UNIFORM_FLOAT;
                    break;

                case REG_TYPE_CONSTINT:
                    type = MOJOSHADER_UNIFORM_INT;
                    break;

                case REG_TYPE_CONSTBOOL:
                    type = MOJOSHADER_UNIFORM_BOOL;
                    break;

                default:
                    fail(ctx, "unknown uniform datatype");
                    break;
            } // switch

            if (!skip)
            {
                wptr->type = type;
                wptr->index = index;
                wptr->array_count = 0;
                wptr->name = alloc_varname(ctx, item);
                wptr++;
                written++;
            } // if

            item = item->next;
        } // for
    } // if

    return retval;
} // build_uniforms


static MOJOSHADER_constant *build_constants(Context *ctx)
{
    const size_t len = sizeof (MOJOSHADER_constant) * ctx->constant_count;
    MOJOSHADER_constant *retval = (MOJOSHADER_constant *) Malloc(ctx, len);

    if (retval != NULL)
    {
        ConstantsList *item = ctx->constants;
        int i;

        for (i = 0; i < ctx->constant_count; i++)
        {
            if (item == NULL)
            {
                fail(ctx, "BUG: mismatched constant list and count");
                break;
            } // if

            memcpy(&retval[i], &item->constant, sizeof (MOJOSHADER_constant));
            item = item->next;
        } // for
    } // if

    return retval;
} // build_constants


static MOJOSHADER_sampler *build_samplers(Context *ctx)
{
    const size_t len = sizeof (MOJOSHADER_sampler) * ctx->sampler_count;
    MOJOSHADER_sampler *retval = (MOJOSHADER_sampler *) Malloc(ctx, len);

    if (retval != NULL)
    {
        RegisterList *item = ctx->samplers.next;
        int i;

        memset(retval, '\0', len);

        for (i = 0; i < ctx->sampler_count; i++)
        {
            if (item == NULL)
            {
                fail(ctx, "BUG: mismatched sampler list and count");
                break;
            } // if

            assert(item->regtype == REG_TYPE_SAMPLER);
            retval[i].type = cvtD3DToMojoSamplerType((TextureType) item->index);
            retval[i].index = item->regnum;
            retval[i].name = alloc_varname(ctx, item);
            retval[i].texbem = (item->misc != 0) ? 1 : 0;
            item = item->next;
        } // for
    } // if

    return retval;
} // build_samplers


static MOJOSHADER_attribute *build_attributes(Context *ctx, int *_count)
{
    int count = 0;

    if (ctx->attribute_count == 0)
    {
        *_count = 0;
        return NULL;  // nothing to do.
    } // if

    const size_t len = sizeof (MOJOSHADER_attribute) * ctx->attribute_count;
    MOJOSHADER_attribute *retval = (MOJOSHADER_attribute *) Malloc(ctx, len);

    if (retval != NULL)
    {
        RegisterList *item = ctx->attributes.next;
        MOJOSHADER_attribute *wptr = retval;
        int ignore = 0;
        int i;

        memset(retval, '\0', len);

        for (i = 0; i < ctx->attribute_count; i++)
        {
            if (item == NULL)
            {
                fail(ctx, "BUG: mismatched attribute list and count");
                break;
            } // if

            switch (item->regtype)
            {
                case REG_TYPE_RASTOUT:
                case REG_TYPE_ATTROUT:
                case REG_TYPE_TEXCRDOUT:
                case REG_TYPE_COLOROUT:
                case REG_TYPE_DEPTHOUT:
                    ignore = 1;
                    break;
                case REG_TYPE_TEXTURE:
                case REG_TYPE_MISCTYPE:
                case REG_TYPE_INPUT:
                    ignore = shader_is_pixel(ctx);
                    break;
                default:
                    ignore = 0;
                    break;
            } // switch

            if (!ignore)
            {
                if (shader_is_pixel(ctx))
                    fail(ctx, "BUG: pixel shader with vertex attributes");
                else
                {
                    wptr->usage = item->usage;
                    wptr->index = item->index;
                    wptr->name = alloc_varname(ctx, item);
                    wptr++;
                    count++;
                } // else
            } // if

            item = item->next;
        } // for
    } // if

    *_count = count;
    return retval;
} // build_attributes

static MOJOSHADER_attribute *build_outputs(Context *ctx, int *_count)
{
    int count = 0;

    if (ctx->attribute_count == 0)
    {
        *_count = 0;
        return NULL;  // nothing to do.
    } // if

    const size_t len = sizeof (MOJOSHADER_attribute) * ctx->attribute_count;
    MOJOSHADER_attribute *retval = (MOJOSHADER_attribute *) Malloc(ctx, len);

    if (retval != NULL)
    {
        RegisterList *item = ctx->attributes.next;
        MOJOSHADER_attribute *wptr = retval;
        int i;

        memset(retval, '\0', len);

        for (i = 0; i < ctx->attribute_count; i++)
        {
            if (item == NULL)
            {
                fail(ctx, "BUG: mismatched attribute list and count");
                break;
            } // if

            switch (item->regtype)
            {
                case REG_TYPE_RASTOUT:
                case REG_TYPE_ATTROUT:
                case REG_TYPE_TEXCRDOUT:
                case REG_TYPE_COLOROUT:
                case REG_TYPE_DEPTHOUT:
                    wptr->usage = item->usage;
                    wptr->index = item->index;
                    wptr->name = alloc_varname(ctx, item);
                    wptr++;
                    count++;
                    break;
                default:
                    break;
            } // switch


            item = item->next;
        } // for
    } // if

    *_count = count;
    return retval;
} // build_outputs


static MOJOSHADER_parseData *build_parsedata(Context *ctx)
{
    char *output = NULL;
    MOJOSHADER_constant *constants = NULL;
    MOJOSHADER_uniform *uniforms = NULL;
    MOJOSHADER_attribute *attributes = NULL;
    MOJOSHADER_attribute *outputs = NULL;
    MOJOSHADER_sampler *samplers = NULL;
    MOJOSHADER_swizzle *swizzles = NULL;
    MOJOSHADER_error *errors = NULL;
    MOJOSHADER_parseData *retval = NULL;
    size_t output_len = 0;
    int attribute_count = 0;
    int output_count = 0;

    if (ctx->out_of_memory)
        return &MOJOSHADER_out_of_mem_data;

    retval = (MOJOSHADER_parseData*) Malloc(ctx, sizeof(MOJOSHADER_parseData));
    if (retval == NULL)
        return &MOJOSHADER_out_of_mem_data;

    memset(retval, '\0', sizeof (MOJOSHADER_parseData));

    if (!isfail(ctx))
        output = build_output(ctx, &output_len);

    if (!isfail(ctx))
        constants = build_constants(ctx);

    if (!isfail(ctx))
        uniforms = build_uniforms(ctx);

    if (!isfail(ctx))
        attributes = build_attributes(ctx, &attribute_count);

    if (!isfail(ctx))
        outputs = build_outputs(ctx, &output_count);

    if (!isfail(ctx))
        samplers = build_samplers(ctx);

    const int error_count = errorlist_count(ctx->errors);
    errors = errorlist_flatten(ctx->errors);

    if (!isfail(ctx))
    {
        if (ctx->swizzles_count > 0)
        {
            const int len = ctx->swizzles_count * sizeof (MOJOSHADER_swizzle);
            swizzles = (MOJOSHADER_swizzle *) Malloc(ctx, len);
            if (swizzles != NULL)
                memcpy(swizzles, ctx->swizzles, len);
        } // if
    } // if

    // check again, in case build_output, etc, ran out of memory.
    if (isfail(ctx))
    {
        int i;

        Free(ctx, output);
        Free(ctx, constants);
        Free(ctx, swizzles);

        if (uniforms != NULL)
        {
            for (i = 0; i < ctx->uniform_count; i++)
                Free(ctx, (void *) uniforms[i].name);
            Free(ctx, uniforms);
        } // if

        if (attributes != NULL)
        {
            for (i = 0; i < attribute_count; i++)
                Free(ctx, (void *) attributes[i].name);
            Free(ctx, attributes);
        } // if

        if (outputs != NULL)
        {
            for (i = 0; i < output_count; i++)
                Free(ctx, (void *) outputs[i].name);
            Free(ctx, outputs);
        } // if

        if (samplers != NULL)
        {
            for (i = 0; i < ctx->sampler_count; i++)
                Free(ctx, (void *) samplers[i].name);
            Free(ctx, samplers);
        } // if

        if (ctx->out_of_memory)
        {
            for (i = 0; i < error_count; i++)
            {
                Free(ctx, (void *) errors[i].filename);
                Free(ctx, (void *) errors[i].error);
            } // for
            Free(ctx, errors);
            Free(ctx, retval);
            return &MOJOSHADER_out_of_mem_data;
        } // if
    } // if
    else
    {
        retval->profile = ctx->profile->name;
        retval->output = output;
        retval->output_len = (int) output_len;
        retval->instruction_count = ctx->instruction_count;
        retval->shader_type = ctx->shader_type;
        retval->major_ver = (int) ctx->major_ver;
        retval->minor_ver = (int) ctx->minor_ver;
        retval->uniform_count = ctx->uniform_count;
        retval->uniforms = uniforms;
        retval->constant_count = ctx->constant_count;
        retval->constants = constants;
        retval->sampler_count = ctx->sampler_count;
        retval->samplers = samplers;
        retval->attribute_count = attribute_count;
        retval->attributes = attributes;
        retval->output_count = output_count;
        retval->outputs = outputs;
        retval->swizzle_count = ctx->swizzles_count;
        retval->swizzles = swizzles;
        retval->symbol_count = ctx->ctab.symbol_count;
        retval->symbols = ctx->ctab.symbols;
        retval->preshader = ctx->preshader;
        retval->mainfn = ctx->mainfn;

        // we don't own these now, retval does.
        ctx->ctab.symbols = NULL;
        ctx->preshader = NULL;
        ctx->ctab.symbol_count = 0;
        ctx->mainfn = NULL;
    } // else

    retval->error_count = error_count;
    retval->errors = errors;
    retval->malloc = (ctx->malloc == MOJOSHADER_internal_malloc) ? NULL : ctx->malloc;
    retval->free = (ctx->free == MOJOSHADER_internal_free) ? NULL : ctx->free;
    retval->malloc_data = ctx->malloc_data;

    return retval;
} // build_parsedata


static void process_definitions(Context *ctx)
{
    // !!! FIXME: apparently, pre ps_3_0, sampler registers don't need to be
    // !!! FIXME:  DCL'd before use (default to 2d?). We aren't checking
    // !!! FIXME:  this at the moment, though.

    determine_constants_arrays(ctx);  // in case this hasn't been called yet.

    RegisterList *uitem = &ctx->uniforms;
    RegisterList *prev = &ctx->used_registers;
    RegisterList *item = prev->next;

    while (item != NULL)
    {
        RegisterList *next = item->next;
        const RegisterType regtype = item->regtype;
        const int regnum = item->regnum;

        if (!get_defined_register(ctx, regtype, regnum))
        {
            // haven't already dealt with this one.
            switch (regtype)
            {
                // !!! FIXME: I'm not entirely sure this is right...
                case REG_TYPE_RASTOUT:
                case REG_TYPE_ATTROUT:
                case REG_TYPE_TEXCRDOUT:
                case REG_TYPE_COLOROUT:
                case REG_TYPE_DEPTHOUT:
                    if (shader_is_vertex(ctx)&&shader_version_atleast(ctx,3,0))
                    {
                        fail(ctx, "vs_3 can't use output registers"
                                  " without declaring them first.");
                        return;
                    } // if

                    // Apparently this is an attribute that wasn't DCL'd.
                    //  Add it to the attribute list; deal with it later.
                    // !!! FIXME: we should use something other than UNKNOWN here.
                    add_attribute_register(ctx, regtype, regnum,
                                           MOJOSHADER_USAGE_UNKNOWN, 0, 0xF, 0);
                    break;

                case REG_TYPE_ADDRESS:
                case REG_TYPE_PREDICATE:
                case REG_TYPE_TEMP:
                case REG_TYPE_LOOP:
                case REG_TYPE_LABEL:
                    ctx->profile->global_emitter(ctx, regtype, regnum);
                    break;

                case REG_TYPE_CONST:
                case REG_TYPE_CONSTINT:
                case REG_TYPE_CONSTBOOL:
                    // separate uniforms into a different list for now.
                    prev->next = next;
                    item->next = NULL;
                    uitem->next = item;
                    uitem = item;
                    item = prev;
                    break;

                case REG_TYPE_INPUT:
                    // You don't have to dcl_ your inputs in Shader Model 1.
                    if (shader_is_pixel(ctx)&&!shader_version_atleast(ctx,2,0))
                    {
                        add_attribute_register(ctx, regtype, regnum,
                                               MOJOSHADER_USAGE_COLOR, regnum,
                                               0xF, 0);
                        break;
                    } // if
                    // fall through...

                default:
                    fail(ctx, "BUG: we used a register we don't know how to define.");
            } // switch
        } // if

        prev = item;
        item = next;
    } // while

    // okay, now deal with uniform/constant arrays...
    for (VariableList *var = ctx->variables; var != NULL; var = var->next)
    {
        if (var->used)
        {
            if (var->constant)
            {
                ctx->profile->const_array_emitter(ctx, var->constant,
                                                  var->index, var->count);
            } // if
            else
            {
                ctx->profile->array_emitter(ctx, var);
                ctx->uniform_float4_count += var->count;
            } // else
            ctx->uniform_count++;
        } // if
    } // for

    // ...and uniforms...
    for (item = ctx->uniforms.next; item != NULL; item = item->next)
    {
        int arraysize = -1;
        VariableList *var = NULL;

        // check if this is a register contained in an array...
        if (item->regtype == REG_TYPE_CONST)
        {
            for (var = ctx->variables; var != NULL; var = var->next)
            {
                if (!var->used)
                    continue;

                const int regnum = item->regnum;
                const int lo = var->index;
                if ( (regnum >= lo) && (regnum < (lo + var->count)) )
                {
                    assert(!var->constant);
                    item->array = var;  // used when building parseData.
                    arraysize = var->count;
                    break;
                } // if
            } // for
        } // if

        ctx->profile->uniform_emitter(ctx, item->regtype, item->regnum, var);

        if (arraysize < 0)  // not part of an array?
        {
            ctx->uniform_count++;
            switch (item->regtype)
            {
                case REG_TYPE_CONST: ctx->uniform_float4_count++; break;
                case REG_TYPE_CONSTINT: ctx->uniform_int4_count++; break;
                case REG_TYPE_CONSTBOOL: ctx->uniform_bool_count++; break;
                default: break;
            } // switch
        } // if
    } // for

    // ...and samplers...
    for (item = ctx->samplers.next; item != NULL; item = item->next)
    {
        ctx->sampler_count++;
        ctx->profile->sampler_emitter(ctx, item->regnum,
                                      (TextureType) item->index,
                                      item->misc != 0);
    } // for

    // ...and attributes...
    for (item = ctx->attributes.next; item != NULL; item = item->next)
    {
        ctx->attribute_count++;
        ctx->profile->attribute_emitter(ctx, item->regtype, item->regnum,
                                        item->usage, item->index,
                                        item->writemask, item->misc);
    } // for
} // process_definitions


static void verify_swizzles(Context *ctx)
{
    size_t i;
    const char *failmsg = "invalid swizzle";
    for (i = 0; i < ctx->swizzles_count; i++)
    {
        const MOJOSHADER_swizzle *swiz = &ctx->swizzles[i];
        if (swiz->swizzles[0] > 3) { fail(ctx, failmsg); return; }
        if (swiz->swizzles[1] > 3) { fail(ctx, failmsg); return; }
        if (swiz->swizzles[2] > 3) { fail(ctx, failmsg); return; }
        if (swiz->swizzles[3] > 3) { fail(ctx, failmsg); return; }
    } // for
} // verify_swizzles


// API entry point...

// !!! FIXME:
// MSDN: "Shader validation will fail CreatePixelShader on any shader that
//  attempts to read from a temporary register that has not been written by a
//  previous instruction."  (true for ps_1_*, maybe others). Check this.

const MOJOSHADER_parseData *MOJOSHADER_parse(const char *profile,
                                             const char *mainfn,
                                             const unsigned char *tokenbuf,
                                             const unsigned int bufsize,
                                             const MOJOSHADER_swizzle *swiz,
                                             const unsigned int swizcount,
                                             const MOJOSHADER_samplerMap *smap,
                                             const unsigned int smapcount,
                                             MOJOSHADER_malloc m,
                                             MOJOSHADER_free f, void *d)
{
    MOJOSHADER_parseData *retval = NULL;
    Context *ctx = NULL;
    int rc = 0;
    int failed = 0;

    if ( ((m == NULL) && (f != NULL)) || ((m != NULL) && (f == NULL)) )
        return &MOJOSHADER_out_of_mem_data;  // supply both or neither.

    ctx = build_context(profile, mainfn, tokenbuf, bufsize, swiz, swizcount,
                        smap, smapcount, m, f, d);
    if (ctx == NULL)
        return &MOJOSHADER_out_of_mem_data;

    if (profile == NULL)  // build_context allows NULL; check this ourselves.
        fail(ctx, "Profile name is NULL");

    if (isfail(ctx))
    {
        retval = build_parsedata(ctx);
        destroy_context(ctx);
        return retval;
    } // if

    verify_swizzles(ctx);

    if (!ctx->mainfn)
        ctx->mainfn = StrDup(ctx, "main");

    // Version token always comes first.
    ctx->current_position = 0;
    rc = parse_version_token(ctx, profile);

    // drop out now if this definitely isn't bytecode. Saves lots of
    //  meaningless errors flooding through.
    if (rc < 0)
    {
        retval = build_parsedata(ctx);
        destroy_context(ctx);
        return retval;
    } // if

    if ( ((uint32) rc) > ctx->tokencount )
    {
        fail(ctx, "Corrupted or truncated shader");
        ctx->tokencount = rc;
    } // if

    adjust_token_position(ctx, rc);

    // parse out the rest of the tokens after the version token...
    while (ctx->tokencount > 0)
    {
        if (!ctx->know_shader_size)
            ctx->tokencount = 0xFFFFFFFF;  // keep this value obscenely large.

        // reset for each token.
        if (isfail(ctx))
        {
            failed = 1;
            ctx->isfail = 0;
        } // if

        rc = parse_token(ctx);
        if ( ((uint32) rc) > ctx->tokencount )
        {
            fail(ctx, "Corrupted or truncated shader");
            break;
        } // if

        adjust_token_position(ctx, rc);
    } // while

    ctx->current_position = MOJOSHADER_POSITION_AFTER;

    // for ps_1_*, the output color is written to r0...throw an
    //  error if this register was never written. This isn't
    //  important for vertex shaders, or shader model 2+.
    if (shader_is_pixel(ctx) && !shader_version_atleast(ctx, 2, 0))
    {
        if (!register_was_written(ctx, REG_TYPE_TEMP, 0))
            fail(ctx, "r0 (pixel shader 1.x color output) never written to");
    } // if

    if (!failed)
    {
        process_definitions(ctx);
        failed = isfail(ctx);
    } // if

    if (!failed)
        ctx->profile->finalize_emitter(ctx);

    ctx->isfail = failed;
    retval = build_parsedata(ctx);
    destroy_context(ctx);
    return retval;
} // MOJOSHADER_parse


void MOJOSHADER_freeParseData(const MOJOSHADER_parseData *_data)
{
    MOJOSHADER_parseData *data = (MOJOSHADER_parseData *) _data;
    if ((data == NULL) || (data == &MOJOSHADER_out_of_mem_data))
        return;  // no-op.

    MOJOSHADER_free f = (data->free == NULL) ? MOJOSHADER_internal_free : data->free;
    void *d = data->malloc_data;
    int i;

    // we don't f(data->profile), because that's internal static data.

    f((void *) data->mainfn, d);
    f((void *) data->output, d);
    f((void *) data->constants, d);
    f((void *) data->swizzles, d);

    for (i = 0; i < data->error_count; i++)
    {
        f((void *) data->errors[i].error, d);
        f((void *) data->errors[i].filename, d);
    } // for
    f((void *) data->errors, d);

    for (i = 0; i < data->uniform_count; i++)
        f((void *) data->uniforms[i].name, d);
    f((void *) data->uniforms, d);

    for (i = 0; i < data->attribute_count; i++)
        f((void *) data->attributes[i].name, d);
    f((void *) data->attributes, d);

    for (i = 0; i < data->output_count; i++)
        f((void *) data->outputs[i].name, d);
    f((void *) data->outputs, d);

    for (i = 0; i < data->sampler_count; i++)
        f((void *) data->samplers[i].name, d);
    f((void *) data->samplers, d);

    free_symbols(f, d, data->symbols, data->symbol_count);
    MOJOSHADER_freePreshader(data->preshader);

    f(data, d);
} // MOJOSHADER_freeParseData


int MOJOSHADER_version(void)
{
    return MOJOSHADER_VERSION;
} // MOJOSHADER_version


const char *MOJOSHADER_changeset(void)
{
    return MOJOSHADER_CHANGESET;
} // MOJOSHADER_changeset


int MOJOSHADER_maxShaderModel(const char *profile)
{
    #define PROFILE_SHADER_MODEL(p,v) if (strcmp(profile, p) == 0) return v;
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_D3D, 3);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_BYTECODE, 3);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_GLSL, 3);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_GLSL120, 3);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_GLSLES, 3);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_ARB1, 2);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_NV2, 2);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_NV3, 2);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_NV4, 3);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_METAL, 3);
    PROFILE_SHADER_MODEL(MOJOSHADER_PROFILE_SPIRV, 3);
    #undef PROFILE_SHADER_MODEL
    return -1;  // unknown profile?
} // MOJOSHADER_maxShaderModel


const MOJOSHADER_preshader *MOJOSHADER_parsePreshader(const unsigned char *buf,
                                                      const unsigned int buflen,
                                                      MOJOSHADER_malloc m,
                                                      MOJOSHADER_free f,
                                                      void *d)
{
    MOJOSHADER_preshader *retval = NULL;

    // We need just enough Context for allocators and error state.
    Context *ctx = build_context(NULL, NULL, buf, buflen, NULL, 0, NULL, 0, m, f, d);
    parse_preshader(ctx, ctx->tokens, ctx->tokencount);
    if (!isfail(ctx))
    {
        retval = ctx->preshader;
        ctx->preshader = NULL;  // don't let destroy_context() eat the retval.
    } // if

    destroy_context(ctx);
    return retval;
} // MOJOSHADER_parsePreshader

void MOJOSHADER_freePreshader(const MOJOSHADER_preshader *preshader)
{
    if (preshader != NULL)
    {
        unsigned int i, j;
        void *d = preshader->malloc_data;
        MOJOSHADER_free f = preshader->free;
        if (f == NULL) f = MOJOSHADER_internal_free;

        f((void *) preshader->literals, d);
        for (i = 0; i < preshader->instruction_count; i++)
        {
            for (j = 0; j < preshader->instructions[i].operand_count; j++)
                f((void *) preshader->instructions[i].operands[j].array_registers, d);
        } // for
        f((void *) preshader->instructions, d);
        f((void *) preshader->registers, d);
        free_symbols(f, d, preshader->symbols, preshader->symbol_count);
        f((void *) preshader, d);
    } // if
} // MOJOSHADER_freePreshader

// end of mojoshader.c ...

