/*
 * QuickJS Javascript Engine
 * 
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include <time.h>
#include <fenv.h>
#include <math.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#endif

#include "cutils.h"
#include "list.h"
#include "quickjs.h"
#include "libregexp.h"
#ifdef CONFIG_BIGNUM
#include "libbf.h"
#endif

#define OPTIMIZE         1
#define SHORT_OPCODES    1
#if defined(EMSCRIPTEN)
#define DIRECT_DISPATCH  0
#else
#define DIRECT_DISPATCH  1
#endif

#if defined(__APPLE__)
#define MALLOC_OVERHEAD  0
#else
#define MALLOC_OVERHEAD  8
#endif

#if !defined(_WIN32)
/* define it if printf uses the RNDN rounding mode instead of RNDNA */
#define CONFIG_PRINTF_RNDN
#endif

/* define to include Atomics.* operations which depend on the OS
   threads */
#if !defined(EMSCRIPTEN)
#define CONFIG_ATOMICS
#endif

#if !defined(EMSCRIPTEN)
/* enable stack limitation */
#define CONFIG_STACK_CHECK
#endif


/* dump object free */
//#define DUMP_FREE
//#define DUMP_CLOSURE
/* dump the bytecode of the compiled functions: combination of bits
   1: dump pass 3 final byte code
   2: dump pass 2 code
   4: dump pass 1 code
   8: dump stdlib functions
  16: dump bytecode in hex
  32: dump line number table
 */
//#define DUMP_BYTECODE  (1)
/* dump the occurence of the automatic GC */
//#define DUMP_GC
/* dump objects freed by the garbage collector */
//#define DUMP_GC_FREE
/* dump objects leaking when freeing the runtime */
//#define DUMP_LEAKS  1
/* dump memory usage before running the garbage collector */
//#define DUMP_MEM
//#define DUMP_OBJECTS    /* dump objects in JS_FreeContext */
//#define DUMP_ATOMS      /* dump atoms in JS_FreeContext */
//#define DUMP_SHAPES     /* dump shapes in JS_FreeContext */
//#define DUMP_MODULE_RESOLVE
//#define DUMP_PROMISE
//#define DUMP_READ_OBJECT

/* test the GC by forcing it before each object allocation */
//#define FORCE_GC_AT_MALLOC

#ifdef CONFIG_ATOMICS
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#endif

enum {
    /* classid tag        */    /* union usage   | properties */
    JS_CLASS_OBJECT = 1,        /* must be first */
    JS_CLASS_ARRAY,             /* u.array       | length */
    JS_CLASS_ERROR,
    JS_CLASS_NUMBER,            /* u.object_data */
    JS_CLASS_STRING,            /* u.object_data */
    JS_CLASS_BOOLEAN,           /* u.object_data */
    JS_CLASS_SYMBOL,            /* u.object_data */
    JS_CLASS_ARGUMENTS,         /* u.array       | length */
    JS_CLASS_MAPPED_ARGUMENTS,  /*               | length */
    JS_CLASS_DATE,              /* u.object_data */
    JS_CLASS_MODULE_NS,
    JS_CLASS_C_FUNCTION,        /* u.cfunc */
    JS_CLASS_BYTECODE_FUNCTION, /* u.func */
    JS_CLASS_BOUND_FUNCTION,    /* u.bound_function */
    JS_CLASS_C_FUNCTION_DATA,   /* u.c_function_data_record */
    JS_CLASS_GENERATOR_FUNCTION, /* u.func */
    JS_CLASS_FOR_IN_ITERATOR,   /* u.for_in_iterator */
    JS_CLASS_REGEXP,            /* u.regexp */
    JS_CLASS_ARRAY_BUFFER,      /* u.array_buffer */
    JS_CLASS_SHARED_ARRAY_BUFFER, /* u.array_buffer */
    JS_CLASS_UINT8C_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_INT8_ARRAY,        /* u.array (typed_array) */
    JS_CLASS_UINT8_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_INT16_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_UINT16_ARRAY,      /* u.array (typed_array) */
    JS_CLASS_INT32_ARRAY,       /* u.array (typed_array) */
    JS_CLASS_UINT32_ARRAY,      /* u.array (typed_array) */
#ifdef CONFIG_BIGNUM
    JS_CLASS_BIG_INT64_ARRAY,   /* u.array (typed_array) */
    JS_CLASS_BIG_UINT64_ARRAY,  /* u.array (typed_array) */
#endif
    JS_CLASS_FLOAT32_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_FLOAT64_ARRAY,     /* u.array (typed_array) */
    JS_CLASS_DATAVIEW,          /* u.typed_array */
#ifdef CONFIG_BIGNUM
    JS_CLASS_BIG_INT,           /* u.object_data */
    JS_CLASS_BIG_FLOAT,         /* u.object_data */
    JS_CLASS_FLOAT_ENV,         /* u.float_env */
    JS_CLASS_BIG_DECIMAL,       /* u.object_data */
    JS_CLASS_OPERATOR_SET,      /* u.operator_set */
#endif
    JS_CLASS_MAP,               /* u.map_state */
    JS_CLASS_SET,               /* u.map_state */
    JS_CLASS_WEAKMAP,           /* u.map_state */
    JS_CLASS_WEAKSET,           /* u.map_state */
    JS_CLASS_MAP_ITERATOR,      /* u.map_iterator_data */
    JS_CLASS_SET_ITERATOR,      /* u.map_iterator_data */
    JS_CLASS_ARRAY_ITERATOR,    /* u.array_iterator_data */
    JS_CLASS_STRING_ITERATOR,   /* u.array_iterator_data */
    JS_CLASS_REGEXP_STRING_ITERATOR,   /* u.regexp_string_iterator_data */
    JS_CLASS_GENERATOR,         /* u.generator_data */
    JS_CLASS_PROXY,             /* u.proxy_data */
    JS_CLASS_PROMISE,           /* u.promise_data */
    JS_CLASS_PROMISE_RESOLVE_FUNCTION,  /* u.promise_function_data */
    JS_CLASS_PROMISE_REJECT_FUNCTION,   /* u.promise_function_data */
    JS_CLASS_ASYNC_FUNCTION,            /* u.func */
    JS_CLASS_ASYNC_FUNCTION_RESOLVE,    /* u.async_function_data */
    JS_CLASS_ASYNC_FUNCTION_REJECT,     /* u.async_function_data */
    JS_CLASS_ASYNC_FROM_SYNC_ITERATOR,  /* u.async_from_sync_iterator_data */
    JS_CLASS_ASYNC_GENERATOR_FUNCTION,  /* u.func */
    JS_CLASS_ASYNC_GENERATOR,   /* u.async_generator_data */

    JS_CLASS_INIT_COUNT, /* last entry for predefined classes */
};

/* number of typed array types */
#define JS_TYPED_ARRAY_COUNT  (JS_CLASS_FLOAT64_ARRAY - JS_CLASS_UINT8C_ARRAY + 1)
static uint8_t const typed_array_size_log2[JS_TYPED_ARRAY_COUNT];
#define typed_array_size_log2(classid)  (typed_array_size_log2[(classid)- JS_CLASS_UINT8C_ARRAY])

typedef enum JSErrorEnum {
    JS_EVAL_ERROR,
    JS_RANGE_ERROR,
    JS_REFERENCE_ERROR,
    JS_SYNTAX_ERROR,
    JS_TYPE_ERROR,
    JS_URI_ERROR,
    JS_INTERNAL_ERROR,
    JS_AGGREGATE_ERROR,
    
    JS_NATIVE_ERROR_COUNT, /* number of different NativeError objects */
} JSErrorEnum;

#define JS_MAX_LOCAL_VARS 65536
#define JS_STACK_SIZE_MAX 65534
#define JS_STRING_LEN_MAX ((1 << 30) - 1)

#define __exception __attribute__((warn_unused_result))

typedef struct JSShape JSShape;
typedef struct JSString JSString;
typedef struct JSString JSAtomStruct;

typedef enum {
    JS_GC_PHASE_NONE,
    JS_GC_PHASE_DECREF,
    JS_GC_PHASE_REMOVE_CYCLES,
} JSGCPhaseEnum;

typedef enum OPCodeEnum OPCodeEnum;

#ifdef CONFIG_BIGNUM
/* function pointers are used for numeric operations so that it is
   possible to remove some numeric types */
typedef struct {
    JSValue (*to_string)(JSContext *ctx, JSValueConst val);
    JSValue (*from_string)(JSContext *ctx, const char *buf,
                           int radix, int flags, slimb_t *pexponent);
    int (*unary_arith)(JSContext *ctx,
                       JSValue *pres, OPCodeEnum op, JSValue op1);
    int (*binary_arith)(JSContext *ctx, OPCodeEnum op,
                        JSValue *pres, JSValue op1, JSValue op2);
    int (*compare)(JSContext *ctx, OPCodeEnum op,
                   JSValue op1, JSValue op2);
    /* only for bigfloat: */
    JSValue (*mul_pow10_to_float64)(JSContext *ctx, const bf_t *a,
                                    int64_t exponent);
    int (*mul_pow10)(JSContext *ctx, JSValue *sp);
} JSNumericOperations;
#endif

struct JSRuntime {
    JSMallocFunctions mf;
    JSMallocState malloc_state;
    const char *rt_info;

    int atom_hash_size; /* power of two */
    int atom_count;
    int atom_size;
    int atom_count_resize; /* resize hash table at this count */
    uint32_t *atom_hash;
    JSAtomStruct **atom_array;
    int atom_free_index; /* 0 = none */

    int class_count;    /* size of class_array */
    JSClass *class_array;

    struct list_head context_list; /* list of JSContext.link */
    /* list of JSGCObjectHeader.link. List of allocated GC objects (used
       by the garbage collector) */
    struct list_head gc_obj_list;
    /* list of JSGCObjectHeader.link. Used during JS_FreeValueRT() */
    struct list_head gc_zero_ref_count_list; 
    struct list_head tmp_obj_list; /* used during GC */
    JSGCPhaseEnum gc_phase : 8;
    size_t malloc_gc_threshold;
#ifdef DUMP_LEAKS
    struct list_head string_list; /* list of JSString.link */
#endif
    /* stack limitation */
    uintptr_t stack_size; /* in bytes, 0 if no limit */
    uintptr_t stack_top;
    uintptr_t stack_limit; /* lower stack limit */
    
    JSValue current_exception;
    /* true if inside an out of memory error, to avoid recursing */
    BOOL in_out_of_memory : 8;

    struct JSStackFrame *current_stack_frame;

    JSInterruptHandler *interrupt_handler;
    void *interrupt_opaque;

    JSHostPromiseRejectionTracker *host_promise_rejection_tracker;
    void *host_promise_rejection_tracker_opaque;
    
    struct list_head job_list; /* list of JSJobEntry.link */

    JSModuleNormalizeFunc *module_normalize_func;
    JSModuleLoaderFunc *module_loader_func;
    void *module_loader_opaque;

    BOOL can_block : 8; /* TRUE if Atomics.wait can block */
    /* used to allocate, free and clone SharedArrayBuffers */
    JSSharedArrayBufferFunctions sab_funcs;
    
    /* Shape hash table */
    int shape_hash_bits;
    int shape_hash_size;
    int shape_hash_count; /* number of hashed shapes */
    JSShape **shape_hash;
#ifdef CONFIG_BIGNUM
    bf_context_t bf_ctx;
    JSNumericOperations bigint_ops;
    JSNumericOperations bigfloat_ops;
    JSNumericOperations bigdecimal_ops;
    uint32_t operator_count;
#endif
    void *user_opaque;
};

struct JSClass {
    uint32_t class_id; /* 0 means free entry */
    JSAtom class_name;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
    JSClassCall *call;
    /* pointers for exotic behavior, can be NULL if none are present */
    const JSClassExoticMethods *exotic;
};

#define JS_MODE_STRICT (1 << 0)
#define JS_MODE_STRIP  (1 << 1)
#define JS_MODE_MATH   (1 << 2)

typedef struct JSStackFrame {
    struct JSStackFrame *prev_frame; /* NULL if first stack frame */
    JSValue cur_func; /* current function, JS_UNDEFINED if the frame is detached */
    JSValue *arg_buf; /* arguments */
    JSValue *var_buf; /* variables */
    struct list_head var_ref_list; /* list of JSVarRef.link */
    const uint8_t *cur_pc; /* only used in bytecode functions : PC of the
                        instruction after the call */
    int arg_count;
    int js_mode; /* 0 or JS_MODE_MATH for C functions */
    /* only used in generators. Current stack pointer value. NULL if
       the function is running. */ 
    JSValue *cur_sp;
} JSStackFrame;

typedef enum {
    JS_GC_OBJ_TYPE_JS_OBJECT,
    JS_GC_OBJ_TYPE_FUNCTION_BYTECODE,
    JS_GC_OBJ_TYPE_SHAPE,
    JS_GC_OBJ_TYPE_VAR_REF,
    JS_GC_OBJ_TYPE_ASYNC_FUNCTION,
    JS_GC_OBJ_TYPE_JS_CONTEXT,
} JSGCObjectTypeEnum;

/* header for GC objects. GC objects are C data structures with a
   reference count that can reference other GC objects. JS Objects are
   a particular type of GC object. */
struct JSGCObjectHeader {
    int ref_count; /* must come first, 32-bit */
    JSGCObjectTypeEnum gc_obj_type : 4;
    uint8_t mark : 4; /* used by the GC */
    uint8_t dummy1; /* not used by the GC */
    uint16_t dummy2; /* not used by the GC */
    struct list_head link;
};

typedef struct JSVarRef {
    union {
        JSGCObjectHeader header; /* must come first */
        struct {
            int __gc_ref_count; /* corresponds to header.ref_count */
            uint8_t __gc_mark; /* corresponds to header.mark/gc_obj_type */

            /* 0 : the JSVarRef is on the stack. header.link is an element
               of JSStackFrame.var_ref_list.
               1 : the JSVarRef is detached. header.link has the normal meanning 
            */
            uint8_t is_detached : 1; 
            uint8_t is_arg : 1;
            uint16_t var_idx; /* index of the corresponding function variable on
                                 the stack */
        };
    };
    JSValue *pvalue; /* pointer to the value, either on the stack or
                        to 'value' */
    JSValue value; /* used when the variable is no longer on the stack */
} JSVarRef;

#ifdef CONFIG_BIGNUM
typedef struct JSFloatEnv {
    limb_t prec;
    bf_flags_t flags;
    unsigned int status;
} JSFloatEnv;

/* the same structure is used for big integers and big floats. Big
   integers are never infinite or NaNs */
typedef struct JSBigFloat {
    JSRefCountHeader header; /* must come first, 32-bit */
    bf_t num;
} JSBigFloat;

typedef struct JSBigDecimal {
    JSRefCountHeader header; /* must come first, 32-bit */
    bfdec_t num;
} JSBigDecimal;
#endif

typedef enum {
    JS_AUTOINIT_ID_PROTOTYPE,
    JS_AUTOINIT_ID_MODULE_NS,
    JS_AUTOINIT_ID_PROP,
} JSAutoInitIDEnum;

/* must be large enough to have a negligible runtime cost and small
   enough to call the interrupt callback often. */
#define JS_INTERRUPT_COUNTER_INIT 10000

struct JSContext {
    JSGCObjectHeader header; /* must come first */
    JSRuntime *rt;
    struct list_head link;

    uint16_t binary_object_count;
    int binary_object_size;

    JSShape *array_shape;   /* initial shape for Array objects */

    JSValue *class_proto;
    JSValue function_proto;
    JSValue function_ctor;
    JSValue array_ctor;
    JSValue regexp_ctor;
    JSValue promise_ctor;
    JSValue native_error_proto[JS_NATIVE_ERROR_COUNT];
    JSValue iterator_proto;
    JSValue async_iterator_proto;
    JSValue array_proto_values;
    JSValue throw_type_error;
    JSValue eval_obj;

    JSValue global_obj; /* global object */
    JSValue global_var_obj; /* contains the global let/const definitions */

    uint64_t random_state;
#ifdef CONFIG_BIGNUM
    bf_context_t *bf_ctx;   /* points to rt->bf_ctx, shared by all contexts */
    JSFloatEnv fp_env; /* global FP environment */
    BOOL bignum_ext : 8; /* enable math mode */
    BOOL allow_operator_overloading : 8;
#endif
    /* when the counter reaches zero, JSRutime.interrupt_handler is called */
    int interrupt_counter;
    BOOL is_error_property_enabled;

    struct list_head loaded_modules; /* list of JSModuleDef.link */

    /* if NULL, RegExp compilation is not supported */
    JSValue (*compile_regexp)(JSContext *ctx, JSValueConst pattern,
                              JSValueConst flags);
    /* if NULL, eval is not supported */
    JSValue (*eval_internal)(JSContext *ctx, JSValueConst this_obj,
                             const char *input, size_t input_len,
                             const char *filename, int flags, int scope_idx);
    void *user_opaque;
};

typedef union JSFloat64Union {
    double d;
    uint64_t u64;
    uint32_t u32[2];
} JSFloat64Union;

enum {
    JS_ATOM_TYPE_STRING = 1,
    JS_ATOM_TYPE_GLOBAL_SYMBOL,
    JS_ATOM_TYPE_SYMBOL,
    JS_ATOM_TYPE_PRIVATE,
};

enum {
    JS_ATOM_HASH_SYMBOL,
    JS_ATOM_HASH_PRIVATE,
};

typedef enum {
    JS_ATOM_KIND_STRING,
    JS_ATOM_KIND_SYMBOL,
    JS_ATOM_KIND_PRIVATE,
} JSAtomKindEnum;

#define JS_ATOM_HASH_MASK  ((1 << 30) - 1)

struct JSString {
    JSRefCountHeader header; /* must come first, 32-bit */
    uint32_t len : 31;
    uint8_t is_wide_char : 1; /* 0 = 8 bits, 1 = 16 bits characters */
    /* for JS_ATOM_TYPE_SYMBOL: hash = 0, atom_type = 3,
       for JS_ATOM_TYPE_PRIVATE: hash = 1, atom_type = 3
       XXX: could change encoding to have one more bit in hash */
    uint32_t hash : 30;
    uint8_t atom_type : 2; /* != 0 if atom, JS_ATOM_TYPE_x */
    uint32_t hash_next; /* atom_index for JS_ATOM_TYPE_SYMBOL */
#ifdef DUMP_LEAKS
    struct list_head link; /* string list */
#endif
    union {
        uint8_t str8[0]; /* 8 bit strings will get an extra null terminator */
        uint16_t str16[0];
    } u;
};

typedef struct JSClosureVar {
    uint8_t is_local : 1;
    uint8_t is_arg : 1;
    uint8_t is_const : 1;
    uint8_t is_lexical : 1;
    uint8_t var_kind : 4; /* see JSVarKindEnum */
    /* 8 bits available */
    uint16_t var_idx; /* is_local = TRUE: index to a normal variable of the
                    parent function. otherwise: index to a closure
                    variable of the parent function */
    JSAtom var_name;
} JSClosureVar;

#define ARG_SCOPE_INDEX 1
#define ARG_SCOPE_END (-2)

typedef struct JSVarScope {
    int parent;  /* index into fd->scopes of the enclosing scope */
    int first;   /* index into fd->vars of the last variable in this scope */
} JSVarScope;

typedef enum {
    /* XXX: add more variable kinds here instead of using bit fields */
    JS_VAR_NORMAL,
    JS_VAR_FUNCTION_DECL, /* lexical var with function declaration */
    JS_VAR_NEW_FUNCTION_DECL, /* lexical var with async/generator
                                 function declaration */
    JS_VAR_CATCH,
    JS_VAR_FUNCTION_NAME, /* function expression name */
    JS_VAR_PRIVATE_FIELD,
    JS_VAR_PRIVATE_METHOD,
    JS_VAR_PRIVATE_GETTER,
    JS_VAR_PRIVATE_SETTER, /* must come after JS_VAR_PRIVATE_GETTER */
    JS_VAR_PRIVATE_GETTER_SETTER, /* must come after JS_VAR_PRIVATE_SETTER */
} JSVarKindEnum;

/* XXX: could use a different structure in bytecode functions to save
   memory */
typedef struct JSVarDef {
    JSAtom var_name;
    /* index into fd->scopes of this variable lexical scope */
    int scope_level;
    /* during compilation: 
        - if scope_level = 0: scope in which the variable is defined
        - if scope_level != 0: index into fd->vars of the next
          variable in the same or enclosing lexical scope
       in a bytecode function:
       index into fd->vars of the next
       variable in the same or enclosing lexical scope
    */
    int scope_next;    
    uint8_t is_const : 1;
    uint8_t is_lexical : 1;
    uint8_t is_captured : 1;
    uint8_t var_kind : 4; /* see JSVarKindEnum */
    /* only used during compilation: function pool index for lexical
       variables with var_kind =
       JS_VAR_FUNCTION_DECL/JS_VAR_NEW_FUNCTION_DECL or scope level of
       the definition of the 'var' variables (they have scope_level =
       0) */
    int func_pool_idx : 24; /* only used during compilation : index in
                               the constant pool for hoisted function
                               definition */
} JSVarDef;

/* for the encoding of the pc2line table */
#define PC2LINE_BASE     (-1)
#define PC2LINE_RANGE    5
#define PC2LINE_OP_FIRST 1
#define PC2LINE_DIFF_PC_MAX ((255 - PC2LINE_OP_FIRST) / PC2LINE_RANGE)

typedef enum JSFunctionKindEnum {
    JS_FUNC_NORMAL = 0,
    JS_FUNC_GENERATOR = (1 << 0),
    JS_FUNC_ASYNC = (1 << 1),
    JS_FUNC_ASYNC_GENERATOR = (JS_FUNC_GENERATOR | JS_FUNC_ASYNC),
} JSFunctionKindEnum;

typedef struct JSFunctionBytecode {
    JSGCObjectHeader header; /* must come first */
    uint8_t js_mode;
    uint8_t has_prototype : 1; /* true if a prototype field is necessary */
    uint8_t has_simple_parameter_list : 1;
    uint8_t is_derived_class_constructor : 1;
    /* true if home_object needs to be initialized */
    uint8_t need_home_object : 1;
    uint8_t func_kind : 2;
    uint8_t new_target_allowed : 1;
    uint8_t super_call_allowed : 1;
    uint8_t super_allowed : 1;
    uint8_t arguments_allowed : 1;
    uint8_t has_debug : 1;
    uint8_t backtrace_barrier : 1; /* stop backtrace on this function */
    uint8_t read_only_bytecode : 1;
    /* XXX: 4 bits available */
    uint8_t *byte_code_buf; /* (self pointer) */
    int byte_code_len;
    JSAtom func_name;
    JSVarDef *vardefs; /* arguments + local variables (arg_count + var_count) (self pointer) */
    JSClosureVar *closure_var; /* list of variables in the closure (self pointer) */
    uint16_t arg_count;
    uint16_t var_count;
    uint16_t defined_arg_count; /* for length function property */
    uint16_t stack_size; /* maximum stack size */
    JSContext *realm; /* function realm */
    JSValue *cpool; /* constant pool (self pointer) */
    int cpool_count;
    int closure_var_count;
    struct {
        /* debug info, move to separate structure to save memory? */
        JSAtom filename;
        int line_num;
        int source_len;
        int pc2line_len;
        uint8_t *pc2line_buf;
        char *source;
    } debug;
} JSFunctionBytecode;

typedef struct JSBoundFunction {
    JSValue func_obj;
    JSValue this_val;
    int argc;
    JSValue argv[0];
} JSBoundFunction;

typedef enum JSIteratorKindEnum {
    JS_ITERATOR_KIND_KEY,
    JS_ITERATOR_KIND_VALUE,
    JS_ITERATOR_KIND_KEY_AND_VALUE,
} JSIteratorKindEnum;

typedef struct JSForInIterator {
    JSValue obj;
    BOOL is_array;
    uint32_t array_length;
    uint32_t idx;
} JSForInIterator;

typedef struct JSRegExp {
    JSString *pattern;
    JSString *bytecode; /* also contains the flags */
} JSRegExp;

typedef struct JSProxyData {
    JSValue target;
    JSValue handler;
    uint8_t is_func;
    uint8_t is_revoked;
} JSProxyData;

typedef struct JSArrayBuffer {
    int byte_length; /* 0 if detached */
    uint8_t detached;
    uint8_t shared; /* if shared, the array buffer cannot be detached */
    uint8_t *data; /* NULL if detached */
    struct list_head array_list;
    void *opaque;
    JSFreeArrayBufferDataFunc *free_func;
} JSArrayBuffer;

typedef struct JSTypedArray {
    struct list_head link; /* link to arraybuffer */
    JSObject *obj; /* back pointer to the TypedArray/DataView object */
    JSObject *buffer; /* based array buffer */
    uint32_t offset; /* offset in the array buffer */
    uint32_t length; /* length in the array buffer */
} JSTypedArray;

typedef struct JSAsyncFunctionState {
    JSValue this_val; /* 'this' generator argument */
    int argc; /* number of function arguments */
    BOOL throw_flag; /* used to throw an exception in JS_CallInternal() */
    JSStackFrame frame;
} JSAsyncFunctionState;

/* XXX: could use an object instead to avoid the
   JS_TAG_ASYNC_FUNCTION tag for the GC */
typedef struct JSAsyncFunctionData {
    JSGCObjectHeader header; /* must come first */
    JSValue resolving_funcs[2];
    BOOL is_active; /* true if the async function state is valid */
    JSAsyncFunctionState func_state;
} JSAsyncFunctionData;

typedef enum {
   /* binary operators */
   JS_OVOP_ADD,
   JS_OVOP_SUB,
   JS_OVOP_MUL,
   JS_OVOP_DIV,
   JS_OVOP_MOD,
   JS_OVOP_POW,
   JS_OVOP_OR,
   JS_OVOP_AND,
   JS_OVOP_XOR,
   JS_OVOP_SHL,
   JS_OVOP_SAR,
   JS_OVOP_SHR,
   JS_OVOP_EQ,
   JS_OVOP_LESS,

   JS_OVOP_BINARY_COUNT,
   /* unary operators */
   JS_OVOP_POS = JS_OVOP_BINARY_COUNT,
   JS_OVOP_NEG,
   JS_OVOP_INC,
   JS_OVOP_DEC,
   JS_OVOP_NOT,

   JS_OVOP_COUNT,
} JSOverloadableOperatorEnum;

typedef struct {
    uint32_t operator_index;
    JSObject *ops[JS_OVOP_BINARY_COUNT]; /* self operators */
} JSBinaryOperatorDefEntry;

typedef struct {
    int count;
    JSBinaryOperatorDefEntry *tab;
} JSBinaryOperatorDef;

typedef struct {
    uint32_t operator_counter;
    BOOL is_primitive; /* OperatorSet for a primitive type */
    /* NULL if no operator is defined */
    JSObject *self_ops[JS_OVOP_COUNT]; /* self operators */
    JSBinaryOperatorDef left;
    JSBinaryOperatorDef right;
} JSOperatorSetData;

typedef struct JSReqModuleEntry {
    JSAtom module_name;
    JSModuleDef *module; /* used using resolution */
} JSReqModuleEntry;

typedef enum JSExportTypeEnum {
    JS_EXPORT_TYPE_LOCAL,
    JS_EXPORT_TYPE_INDIRECT,
} JSExportTypeEnum;

typedef struct JSExportEntry {
    union {
        struct {
            int var_idx; /* closure variable index */
            JSVarRef *var_ref; /* if != NULL, reference to the variable */
        } local; /* for local export */
        int req_module_idx; /* module for indirect export */
    } u;
    JSExportTypeEnum export_type;
    JSAtom local_name; /* '*' if export ns from. not used for local
                          export after compilation */
    JSAtom export_name; /* exported variable name */
} JSExportEntry;

typedef struct JSStarExportEntry {
    int req_module_idx; /* in req_module_entries */
} JSStarExportEntry;

typedef struct JSImportEntry {
    int var_idx; /* closure variable index */
    JSAtom import_name;
    int req_module_idx; /* in req_module_entries */
} JSImportEntry;

struct JSModuleDef {
    JSRefCountHeader header; /* must come first, 32-bit */
    JSAtom module_name;
    struct list_head link;

    JSReqModuleEntry *req_module_entries;
    int req_module_entries_count;
    int req_module_entries_size;

    JSExportEntry *export_entries;
    int export_entries_count;
    int export_entries_size;

    JSStarExportEntry *star_export_entries;
    int star_export_entries_count;
    int star_export_entries_size;

    JSImportEntry *import_entries;
    int import_entries_count;
    int import_entries_size;

    JSValue module_ns;
    JSValue func_obj; /* only used for JS modules */
    JSModuleInitFunc *init_func; /* only used for C modules */
    BOOL resolved : 8;
    BOOL func_created : 8;
    BOOL instantiated : 8;
    BOOL evaluated : 8;
    BOOL eval_mark : 8; /* temporary use during js_evaluate_module() */
    /* true if evaluation yielded an exception. It is saved in
       eval_exception */
    BOOL eval_has_exception : 8; 
    JSValue eval_exception;
    JSValue meta_obj; /* for import.meta */
};

typedef struct JSJobEntry {
    struct list_head link;
    JSContext *ctx;
    JSJobFunc *job_func;
    int argc;
    JSValue argv[0];
} JSJobEntry;

typedef struct JSProperty {
    union {
        JSValue value;      /* JS_PROP_NORMAL */
        struct {            /* JS_PROP_GETSET */
            JSObject *getter; /* NULL if undefined */
            JSObject *setter; /* NULL if undefined */
        } getset;
        JSVarRef *var_ref;  /* JS_PROP_VARREF */
        struct {            /* JS_PROP_AUTOINIT */
            /* in order to use only 2 pointers, we compress the realm
               and the init function pointer */
            uintptr_t realm_and_id; /* realm and init_id (JS_AUTOINIT_ID_x)
                                       in the 2 low bits */
            void *opaque;
        } init;
    } u;
} JSProperty;

#define JS_PROP_INITIAL_SIZE 2
#define JS_PROP_INITIAL_HASH_SIZE 4 /* must be a power of two */
#define JS_ARRAY_INITIAL_SIZE 2

typedef struct JSShapeProperty {
    uint32_t hash_next : 26; /* 0 if last in list */
    uint32_t flags : 6;   /* JS_PROP_XXX */
    JSAtom atom; /* JS_ATOM_NULL = free property entry */
} JSShapeProperty;

struct JSShape {
    /* hash table of size hash_mask + 1 before the start of the
       structure (see prop_hash_end()). */
    JSGCObjectHeader header;
    /* true if the shape is inserted in the shape hash table. If not,
       JSShape.hash is not valid */
    uint8_t is_hashed;
    /* If true, the shape may have small array index properties 'n' with 0
       <= n <= 2^31-1. If false, the shape is guaranteed not to have
       small array index properties */
    uint8_t has_small_array_index;
    uint32_t hash; /* current hash value */
    uint32_t prop_hash_mask;
    int prop_size; /* allocated properties */
    int prop_count; /* include deleted properties */
    int deleted_prop_count;
    JSShape *shape_hash_next; /* in JSRuntime.shape_hash[h] list */
    JSObject *proto;
    JSShapeProperty prop[0]; /* prop_size elements */
};

struct JSObject {
    union {
        JSGCObjectHeader header;
        struct {
            int __gc_ref_count; /* corresponds to header.ref_count */
            uint8_t __gc_mark; /* corresponds to header.mark/gc_obj_type */
            
            uint8_t extensible : 1;
            uint8_t free_mark : 1; /* only used when freeing objects with cycles */
            uint8_t is_exotic : 1; /* TRUE if object has exotic property handlers */
            uint8_t fast_array : 1; /* TRUE if u.array is used for get/put (for JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS and typed arrays) */
            uint8_t is_constructor : 1; /* TRUE if object is a constructor function */
            uint8_t is_uncatchable_error : 1; /* if TRUE, error is not catchable */
            uint8_t tmp_mark : 1; /* used in JS_WriteObjectRec() */
            uint8_t is_HTMLDDA : 1; /* specific annex B IsHtmlDDA behavior */
            uint16_t class_id; /* see JS_CLASS_x */
        };
    };
    /* byte offsets: 16/24 */
    JSShape *shape; /* prototype and property names + flag */
    JSProperty *prop; /* array of properties */
    /* byte offsets: 24/40 */
    struct JSMapRecord *first_weak_ref; /* XXX: use a bit and an external hash table? */
    /* byte offsets: 28/48 */
    union {
        void *opaque;
        struct JSBoundFunction *bound_function; /* JS_CLASS_BOUND_FUNCTION */
        struct JSCFunctionDataRecord *c_function_data_record; /* JS_CLASS_C_FUNCTION_DATA */
        struct JSForInIterator *for_in_iterator; /* JS_CLASS_FOR_IN_ITERATOR */
        struct JSArrayBuffer *array_buffer; /* JS_CLASS_ARRAY_BUFFER, JS_CLASS_SHARED_ARRAY_BUFFER */
        struct JSTypedArray *typed_array; /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_DATAVIEW */
#ifdef CONFIG_BIGNUM
        struct JSFloatEnv *float_env; /* JS_CLASS_FLOAT_ENV */
        struct JSOperatorSetData *operator_set; /* JS_CLASS_OPERATOR_SET */
#endif
        struct JSMapState *map_state;   /* JS_CLASS_MAP..JS_CLASS_WEAKSET */
        struct JSMapIteratorData *map_iterator_data; /* JS_CLASS_MAP_ITERATOR, JS_CLASS_SET_ITERATOR */
        struct JSArrayIteratorData *array_iterator_data; /* JS_CLASS_ARRAY_ITERATOR, JS_CLASS_STRING_ITERATOR */
        struct JSRegExpStringIteratorData *regexp_string_iterator_data; /* JS_CLASS_REGEXP_STRING_ITERATOR */
        struct JSGeneratorData *generator_data; /* JS_CLASS_GENERATOR */
        struct JSProxyData *proxy_data; /* JS_CLASS_PROXY */
        struct JSPromiseData *promise_data; /* JS_CLASS_PROMISE */
        struct JSPromiseFunctionData *promise_function_data; /* JS_CLASS_PROMISE_RESOLVE_FUNCTION, JS_CLASS_PROMISE_REJECT_FUNCTION */
        struct JSAsyncFunctionData *async_function_data; /* JS_CLASS_ASYNC_FUNCTION_RESOLVE, JS_CLASS_ASYNC_FUNCTION_REJECT */
        struct JSAsyncFromSyncIteratorData *async_from_sync_iterator_data; /* JS_CLASS_ASYNC_FROM_SYNC_ITERATOR */
        struct JSAsyncGeneratorData *async_generator_data; /* JS_CLASS_ASYNC_GENERATOR */
        struct { /* JS_CLASS_BYTECODE_FUNCTION: 12/24 bytes */
            /* also used by JS_CLASS_GENERATOR_FUNCTION, JS_CLASS_ASYNC_FUNCTION and JS_CLASS_ASYNC_GENERATOR_FUNCTION */
            struct JSFunctionBytecode *function_bytecode;
            JSVarRef **var_refs;
            JSObject *home_object; /* for 'super' access */
        } func;
        struct { /* JS_CLASS_C_FUNCTION: 12/20 bytes */
            JSContext *realm;
            JSCFunctionType c_function;
            uint8_t length;
            uint8_t cproto;
            int16_t magic;
        } cfunc;
        /* array part for fast arrays and typed arrays */
        struct { /* JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS, JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
            union {
                uint32_t size;          /* JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS */
                struct JSTypedArray *typed_array; /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
            } u1;
            union {
                JSValue *values;        /* JS_CLASS_ARRAY, JS_CLASS_ARGUMENTS */ 
                void *ptr;              /* JS_CLASS_UINT8C_ARRAY..JS_CLASS_FLOAT64_ARRAY */
                int8_t *int8_ptr;       /* JS_CLASS_INT8_ARRAY */
                uint8_t *uint8_ptr;     /* JS_CLASS_UINT8_ARRAY, JS_CLASS_UINT8C_ARRAY */
                int16_t *int16_ptr;     /* JS_CLASS_INT16_ARRAY */
                uint16_t *uint16_ptr;   /* JS_CLASS_UINT16_ARRAY */
                int32_t *int32_ptr;     /* JS_CLASS_INT32_ARRAY */
                uint32_t *uint32_ptr;   /* JS_CLASS_UINT32_ARRAY */
                int64_t *int64_ptr;     /* JS_CLASS_INT64_ARRAY */
                uint64_t *uint64_ptr;   /* JS_CLASS_UINT64_ARRAY */
                float *float_ptr;       /* JS_CLASS_FLOAT32_ARRAY */
                double *double_ptr;     /* JS_CLASS_FLOAT64_ARRAY */
            } u;
            uint32_t count; /* <= 2^31-1. 0 for a detached typed array */
        } array;    /* 12/20 bytes */
        JSRegExp regexp;    /* JS_CLASS_REGEXP: 8/16 bytes */
        JSValue object_data;    /* for JS_SetObjectData(): 8/16/16 bytes */
    } u;
    /* byte sizes: 40/48/72 */
};
enum {
    __JS_ATOM_NULL = JS_ATOM_NULL,
#define DEF(name, str) JS_ATOM_ ## name,
#include "quickjs-atom.h"
#undef DEF
    JS_ATOM_END,
};
#define JS_ATOM_LAST_KEYWORD JS_ATOM_super
#define JS_ATOM_LAST_STRICT_KEYWORD JS_ATOM_yield

static const char js_atom_init[] =
#define DEF(name, str) str "\0"
#include "quickjs-atom.h"
#undef DEF
;

typedef enum OPCodeFormat {
#define FMT(f) OP_FMT_ ## f,
#define DEF(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef DEF
#undef FMT
} OPCodeFormat;

enum OPCodeEnum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_ ## id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_COUNT, /* excluding temporary opcodes */
    /* temporary opcodes : overlap with the short opcodes */
    OP_TEMP_START = OP_nop + 1,
    OP___dummy = OP_TEMP_START - 1,
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f)
#define def(id, size, n_pop, n_push, f) OP_ ## id,
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_TEMP_END,
};

static int JS_InitAtoms(JSRuntime *rt);
static JSAtom __JS_NewAtomInit(JSRuntime *rt, const char *str, int len,
                               int atom_type);
static void JS_FreeAtomStruct(JSRuntime *rt, JSAtomStruct *p);
static void free_function_bytecode(JSRuntime *rt, JSFunctionBytecode *b);
static JSValue js_call_c_function(JSContext *ctx, JSValueConst func_obj,
                                  JSValueConst this_obj,
                                  int argc, JSValueConst *argv, int flags);
static JSValue js_call_bound_function(JSContext *ctx, JSValueConst func_obj,
                                      JSValueConst this_obj,
                                      int argc, JSValueConst *argv, int flags);
static JSValue JS_CallInternal(JSContext *ctx, JSValueConst func_obj,
                               JSValueConst this_obj, JSValueConst new_target,
                               int argc, JSValue *argv, int flags);
static JSValue JS_CallConstructorInternal(JSContext *ctx,
                                          JSValueConst func_obj,
                                          JSValueConst new_target,
                                          int argc, JSValue *argv, int flags);
static JSValue JS_CallFree(JSContext *ctx, JSValue func_obj, JSValueConst this_obj,
                           int argc, JSValueConst *argv);
static JSValue JS_InvokeFree(JSContext *ctx, JSValue this_val, JSAtom atom,
                             int argc, JSValueConst *argv);
static __exception int JS_ToArrayLengthFree(JSContext *ctx, uint32_t *plen,
                                            JSValue val, BOOL is_array_ctor);
static JSValue JS_EvalObject(JSContext *ctx, JSValueConst this_obj,
                             JSValueConst val, int flags, int scope_idx);
JSValue __attribute__((format(printf, 2, 3))) JS_ThrowInternalError(JSContext *ctx, const char *fmt, ...);
static __maybe_unused void JS_DumpAtoms(JSRuntime *rt);
static __maybe_unused void JS_DumpString(JSRuntime *rt,
                                                  const JSString *p);
static __maybe_unused void JS_DumpObjectHeader(JSRuntime *rt);
static __maybe_unused void JS_DumpObject(JSRuntime *rt, JSObject *p);
static __maybe_unused void JS_DumpGCObject(JSRuntime *rt, JSGCObjectHeader *p);
static __maybe_unused void JS_DumpValueShort(JSRuntime *rt,
                                                      JSValueConst val);
static __maybe_unused void JS_DumpValue(JSContext *ctx, JSValueConst val);
static __maybe_unused void JS_PrintValue(JSContext *ctx,
                                                  const char *str,
                                                  JSValueConst val);
static __maybe_unused void JS_DumpShapes(JSRuntime *rt);
static JSValue js_function_apply(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic);
static void js_array_finalizer(JSRuntime *rt, JSValue val);
static void js_array_mark(JSRuntime *rt, JSValueConst val,
                          JS_MarkFunc *mark_func);
static void js_object_data_finalizer(JSRuntime *rt, JSValue val);
static void js_object_data_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_c_function_finalizer(JSRuntime *rt, JSValue val);
static void js_c_function_mark(JSRuntime *rt, JSValueConst val,
                               JS_MarkFunc *mark_func);
static void js_bytecode_function_finalizer(JSRuntime *rt, JSValue val);
static void js_bytecode_function_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_bound_function_finalizer(JSRuntime *rt, JSValue val);
static void js_bound_function_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_for_in_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_for_in_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_regexp_finalizer(JSRuntime *rt, JSValue val);
static void js_array_buffer_finalizer(JSRuntime *rt, JSValue val);
static void js_typed_array_finalizer(JSRuntime *rt, JSValue val);
static void js_typed_array_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_proxy_finalizer(JSRuntime *rt, JSValue val);
static void js_proxy_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_map_finalizer(JSRuntime *rt, JSValue val);
static void js_map_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_map_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_map_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_array_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_array_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_regexp_string_iterator_finalizer(JSRuntime *rt, JSValue val);
static void js_regexp_string_iterator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_generator_finalizer(JSRuntime *rt, JSValue obj);
static void js_generator_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_promise_finalizer(JSRuntime *rt, JSValue val);
static void js_promise_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
static void js_promise_resolve_function_finalizer(JSRuntime *rt, JSValue val);
static void js_promise_resolve_function_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func);
#ifdef CONFIG_BIGNUM
static void js_operator_set_finalizer(JSRuntime *rt, JSValue val);
static void js_operator_set_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func);
#endif
static JSValue JS_ToStringFree(JSContext *ctx, JSValue val);
static int JS_ToBoolFree(JSContext *ctx, JSValue val);
static int JS_ToInt32Free(JSContext *ctx, int32_t *pres, JSValue val);
static int JS_ToFloat64Free(JSContext *ctx, double *pres, JSValue val);
static int JS_ToUint8ClampFree(JSContext *ctx, int32_t *pres, JSValue val);
static JSValue js_compile_regexp(JSContext *ctx, JSValueConst pattern,
                                 JSValueConst flags);
static JSValue js_regexp_constructor_internal(JSContext *ctx, JSValueConst ctor,
                                              JSValue pattern, JSValue bc);
static void gc_decref(JSRuntime *rt);
static int JS_NewClass1(JSRuntime *rt, JSClassID class_id,
                        const JSClassDef *class_def, JSAtom name);

typedef enum JSStrictEqModeEnum {
    JS_EQ_STRICT,
    JS_EQ_SAME_VALUE,
    JS_EQ_SAME_VALUE_ZERO,
} JSStrictEqModeEnum;

static BOOL js_strict_eq2(JSContext *ctx, JSValue op1, JSValue op2,
                          JSStrictEqModeEnum eq_mode);
static BOOL js_strict_eq(JSContext *ctx, JSValue op1, JSValue op2);
static BOOL js_same_value(JSContext *ctx, JSValueConst op1, JSValueConst op2);
static BOOL js_same_value_zero(JSContext *ctx, JSValueConst op1, JSValueConst op2);
static JSValue JS_ToObject(JSContext *ctx, JSValueConst val);
static JSValue JS_ToObjectFree(JSContext *ctx, JSValue val);
static JSProperty *add_property(JSContext *ctx,
                                JSObject *p, JSAtom prop, int prop_flags);
#ifdef CONFIG_BIGNUM
static void js_float_env_finalizer(JSRuntime *rt, JSValue val);
static JSValue JS_NewBigFloat(JSContext *ctx);
static inline bf_t *JS_GetBigFloat(JSValueConst val)
{
    JSBigFloat *p = JS_VALUE_GET_PTR(val);
    return &p->num;
}
static JSValue JS_NewBigDecimal(JSContext *ctx);
static inline bfdec_t *JS_GetBigDecimal(JSValueConst val)
{
    JSBigDecimal *p = JS_VALUE_GET_PTR(val);
    return &p->num;
}
static JSValue JS_NewBigInt(JSContext *ctx);
static inline bf_t *JS_GetBigInt(JSValueConst val)
{
    JSBigFloat *p = JS_VALUE_GET_PTR(val);
    return &p->num;
}
static JSValue JS_CompactBigInt1(JSContext *ctx, JSValue val,
                                 BOOL convert_to_safe_integer);
static JSValue JS_CompactBigInt(JSContext *ctx, JSValue val);
static int JS_ToBigInt64Free(JSContext *ctx, int64_t *pres, JSValue val);
static bf_t *JS_ToBigInt(JSContext *ctx, bf_t *buf, JSValueConst val);
static void JS_FreeBigInt(JSContext *ctx, bf_t *a, bf_t *buf);
static bf_t *JS_ToBigFloat(JSContext *ctx, bf_t *buf, JSValueConst val);
static JSValue JS_ToBigDecimalFree(JSContext *ctx, JSValue val,
                                   BOOL allow_null_or_undefined);
static bfdec_t *JS_ToBigDecimal(JSContext *ctx, JSValueConst val);
#endif
JSValue JS_ThrowOutOfMemory(JSContext *ctx);
static JSValue JS_ThrowTypeErrorRevokedProxy(JSContext *ctx);
static JSValue js_proxy_getPrototypeOf(JSContext *ctx, JSValueConst obj);
static int js_proxy_setPrototypeOf(JSContext *ctx, JSValueConst obj,
                                   JSValueConst proto_val, BOOL throw_flag);
static int js_proxy_isExtensible(JSContext *ctx, JSValueConst obj);
static int js_proxy_preventExtensions(JSContext *ctx, JSValueConst obj);
static int js_proxy_isArray(JSContext *ctx, JSValueConst obj);
static int JS_CreateProperty(JSContext *ctx, JSObject *p,
                             JSAtom prop, JSValueConst val,
                             JSValueConst getter, JSValueConst setter,
                             int flags);
static int js_string_memcmp(const JSString *p1, const JSString *p2, int len);
static void reset_weak_ref(JSRuntime *rt, JSObject *p);
static JSValue js_array_buffer_constructor3(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len, JSClassID class_id,
                                            uint8_t *buf,
                                            JSFreeArrayBufferDataFunc *free_func,
                                            void *opaque, BOOL alloc_flag);
static JSArrayBuffer *js_get_array_buffer(JSContext *ctx, JSValueConst obj);
static JSValue js_typed_array_constructor(JSContext *ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int classid);
static BOOL typed_array_is_detached(JSContext *ctx, JSObject *p);
static uint32_t typed_array_get_length(JSContext *ctx, JSObject *p);
static JSValue JS_ThrowTypeErrorDetachedArrayBuffer(JSContext *ctx);
static JSVarRef *get_var_ref(JSContext *ctx, JSStackFrame *sf, int var_idx,
                             BOOL is_arg);
static JSValue js_generator_function_call(JSContext *ctx, JSValueConst func_obj,
                                          JSValueConst this_obj,
                                          int argc, JSValueConst *argv,
                                          int flags);
static void js_async_function_resolve_finalizer(JSRuntime *rt, JSValue val);
static void js_async_function_resolve_mark(JSRuntime *rt, JSValueConst val,
                                           JS_MarkFunc *mark_func);
static JSValue JS_EvalInternal(JSContext *ctx, JSValueConst this_obj,
                               const char *input, size_t input_len,
                               const char *filename, int flags, int scope_idx);
static void js_free_module_def(JSContext *ctx, JSModuleDef *m);
static void js_mark_module_def(JSRuntime *rt, JSModuleDef *m,
                               JS_MarkFunc *mark_func);
static JSValue js_import_meta(JSContext *ctx);
static JSValue js_dynamic_import(JSContext *ctx, JSValueConst specifier);
static void free_var_ref(JSRuntime *rt, JSVarRef *var_ref);
static JSValue js_new_promise_capability(JSContext *ctx,
                                         JSValue *resolving_funcs,
                                         JSValueConst ctor);
static __exception int perform_promise_then(JSContext *ctx,
                                            JSValueConst promise,
                                            JSValueConst *resolve_reject,
                                            JSValueConst *cap_resolving_funcs);
static JSValue js_promise_resolve(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic);
static int js_string_compare(JSContext *ctx,
                             const JSString *p1, const JSString *p2);
static JSValue JS_ToNumber(JSContext *ctx, JSValueConst val);
static int JS_SetPropertyValue(JSContext *ctx, JSValueConst this_obj,
                               JSValue prop, JSValue val, int flags);
static int JS_NumberIsInteger(JSContext *ctx, JSValueConst val);
static BOOL JS_NumberIsNegativeOrMinusZero(JSContext *ctx, JSValueConst val);
static JSValue JS_ToNumberFree(JSContext *ctx, JSValue val);
static int JS_GetOwnPropertyInternal(JSContext *ctx, JSPropertyDescriptor *desc,
                                     JSObject *p, JSAtom prop);
static void js_free_desc(JSContext *ctx, JSPropertyDescriptor *desc);
static void async_func_mark(JSRuntime *rt, JSAsyncFunctionState *s,
                            JS_MarkFunc *mark_func);
static void JS_AddIntrinsicBasicObjects(JSContext *ctx);
static void js_free_shape(JSRuntime *rt, JSShape *sh);
static void js_free_shape_null(JSRuntime *rt, JSShape *sh);
static int js_shape_prepare_update(JSContext *ctx, JSObject *p,
                                   JSShapeProperty **pprs);
static int init_shape_hash(JSRuntime *rt);
static __exception int js_get_length32(JSContext *ctx, uint32_t *pres,
                                       JSValueConst obj);
static __exception int js_get_length64(JSContext *ctx, int64_t *pres,
                                       JSValueConst obj);
static void free_arg_list(JSContext *ctx, JSValue *tab, uint32_t len);
static JSValue *build_arg_list(JSContext *ctx, uint32_t *plen,
                               JSValueConst array_arg);
static BOOL js_get_fast_array(JSContext *ctx, JSValueConst obj,
                              JSValue **arrpp, uint32_t *countp);
static JSValue JS_CreateAsyncFromSyncIterator(JSContext *ctx,
                                              JSValueConst sync_iter);
static void js_c_function_data_finalizer(JSRuntime *rt, JSValue val);
static void js_c_function_data_mark(JSRuntime *rt, JSValueConst val,
                                    JS_MarkFunc *mark_func);
static JSValue js_c_function_data_call(JSContext *ctx, JSValueConst func_obj,
                                       JSValueConst this_val,
                                       int argc, JSValueConst *argv, int flags);
static JSAtom js_symbol_to_atom(JSContext *ctx, JSValue val);
static void add_gc_object(JSRuntime *rt, JSGCObjectHeader *h,
                          JSGCObjectTypeEnum type);
static void remove_gc_object(JSGCObjectHeader *h);
static void js_async_function_free0(JSRuntime *rt, JSAsyncFunctionData *s);
static JSValue js_instantiate_prototype(JSContext *ctx, JSObject *p, JSAtom atom, void *opaque);
static JSValue js_module_ns_autoinit(JSContext *ctx, JSObject *p, JSAtom atom,
                                 void *opaque);
static JSValue JS_InstantiateFunctionListItem2(JSContext *ctx, JSObject *p,
                                               JSAtom atom, void *opaque);
void JS_SetUncatchableError(JSContext *ctx, JSValueConst val, BOOL flag);

static const JSClassExoticMethods js_arguments_exotic_methods;
static const JSClassExoticMethods js_string_exotic_methods;
static const JSClassExoticMethods js_proxy_exotic_methods;
static const JSClassExoticMethods js_module_ns_exotic_methods;
static JSClassID js_class_id_alloc = JS_CLASS_INIT_COUNT;

static void js_trigger_gc(JSRuntime *rt, size_t size)
{
    BOOL force_gc;
#ifdef FORCE_GC_AT_MALLOC
    force_gc = TRUE;
#else
    force_gc = ((rt->malloc_state.malloc_size + size) >
                rt->malloc_gc_threshold);
#endif
    if (force_gc) {
#ifdef DUMP_GC
        printf("GC: size=%" PRIu64 "\n",
               (uint64_t)rt->malloc_state.malloc_size);
#endif
        JS_RunGC(rt);
        rt->malloc_gc_threshold = rt->malloc_state.malloc_size +
            (rt->malloc_state.malloc_size >> 1);
    }
}

static size_t js_malloc_usable_size_unknown(const void *ptr)
{
    return 0;
}

void *js_malloc_rt(JSRuntime *rt, size_t size)
{
    return rt->mf.js_malloc(&rt->malloc_state, size);
}

void js_free_rt(JSRuntime *rt, void *ptr)
{
    rt->mf.js_free(&rt->malloc_state, ptr);
}

void *js_realloc_rt(JSRuntime *rt, void *ptr, size_t size)
{
    return rt->mf.js_realloc(&rt->malloc_state, ptr, size);
}

size_t js_malloc_usable_size_rt(JSRuntime *rt, const void *ptr)
{
    return rt->mf.js_malloc_usable_size(ptr);
}

void *js_mallocz_rt(JSRuntime *rt, size_t size)
{
    void *ptr;
    ptr = js_malloc_rt(rt, size);
    if (!ptr)
        return NULL;
    return memset(ptr, 0, size);
}

#ifdef CONFIG_BIGNUM
/* called by libbf */
static void *js_bf_realloc(void *opaque, void *ptr, size_t size)
{
    JSRuntime *rt = opaque;
    return js_realloc_rt(rt, ptr, size);
}
#endif /* CONFIG_BIGNUM */

/* Throw out of memory in case of error */
void *js_malloc(JSContext *ctx, size_t size)
{
    void *ptr;
    ptr = js_malloc_rt(ctx->rt, size);
    if (unlikely(!ptr)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return ptr;
}

/* Throw out of memory in case of error */
void *js_mallocz(JSContext *ctx, size_t size)
{
    void *ptr;
    ptr = js_mallocz_rt(ctx->rt, size);
    if (unlikely(!ptr)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return ptr;
}

void js_free(JSContext *ctx, void *ptr)
{
    js_free_rt(ctx->rt, ptr);
}

/* Throw out of memory in case of error */
void *js_realloc(JSContext *ctx, void *ptr, size_t size)
{
    void *ret;
    ret = js_realloc_rt(ctx->rt, ptr, size);
    if (unlikely(!ret && size != 0)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return ret;
}

/* store extra allocated size in *pslack if successful */
void *js_realloc2(JSContext *ctx, void *ptr, size_t size, size_t *pslack)
{
    void *ret;
    ret = js_realloc_rt(ctx->rt, ptr, size);
    if (unlikely(!ret && size != 0)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    if (pslack) {
        size_t new_size = js_malloc_usable_size_rt(ctx->rt, ret);
        *pslack = (new_size > size) ? new_size - size : 0;
    }
    return ret;
}

size_t js_malloc_usable_size(JSContext *ctx, const void *ptr)
{
    return js_malloc_usable_size_rt(ctx->rt, ptr);
}

/* Throw out of memory exception in case of error */
char *js_strndup(JSContext *ctx, const char *s, size_t n)
{
    char *ptr;
    ptr = js_malloc(ctx, n + 1);
    if (ptr) {
        memcpy(ptr, s, n);
        ptr[n] = '\0';
    }
    return ptr;
}

char *js_strdup(JSContext *ctx, const char *str)
{
    return js_strndup(ctx, str, strlen(str));
}

static no_inline int js_realloc_array(JSContext *ctx, void **parray,
                                      int elem_size, int *psize, int req_size)
{
    int new_size;
    size_t slack;
    void *new_array;
    /* XXX: potential arithmetic overflow */
    new_size = max_int(req_size, *psize * 3 / 2);
    new_array = js_realloc2(ctx, *parray, new_size * elem_size, &slack);
    if (!new_array)
        return -1;
    new_size += slack / elem_size;
    *psize = new_size;
    *parray = new_array;
    return 0;
}

/* resize the array and update its size if req_size > *psize */
static inline int js_resize_array(JSContext *ctx, void **parray, int elem_size,
                                  int *psize, int req_size)
{
    if (unlikely(req_size > *psize))
        return js_realloc_array(ctx, parray, elem_size, psize, req_size);
    else
        return 0;
}

static inline void js_dbuf_init(JSContext *ctx, DynBuf *s)
{
    dbuf_init2(s, ctx->rt, (DynBufReallocFunc *)js_realloc_rt);
}

static inline int is_digit(int c) {
    return c >= '0' && c <= '9';
}

typedef struct JSClassShortDef {
    JSAtom class_name;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
} JSClassShortDef;

static JSClassShortDef const js_std_class_def[] = {
    { JS_ATOM_Object, NULL, NULL },                             /* JS_CLASS_OBJECT */
    { JS_ATOM_Array, js_array_finalizer, js_array_mark },       /* JS_CLASS_ARRAY */
    { JS_ATOM_Error, NULL, NULL }, /* JS_CLASS_ERROR */
    { JS_ATOM_Number, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_NUMBER */
    { JS_ATOM_String, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_STRING */
    { JS_ATOM_Boolean, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_BOOLEAN */
    { JS_ATOM_Symbol, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_SYMBOL */
    { JS_ATOM_Arguments, js_array_finalizer, js_array_mark },   /* JS_CLASS_ARGUMENTS */
    { JS_ATOM_Arguments, NULL, NULL },                          /* JS_CLASS_MAPPED_ARGUMENTS */
    { JS_ATOM_Date, js_object_data_finalizer, js_object_data_mark }, /* JS_CLASS_DATE */
    { JS_ATOM_Object, NULL, NULL },                             /* JS_CLASS_MODULE_NS */
    { JS_ATOM_Function, js_c_function_finalizer, js_c_function_mark }, /* JS_CLASS_C_FUNCTION */
    { JS_ATOM_Function, js_bytecode_function_finalizer, js_bytecode_function_mark }, /* JS_CLASS_BYTECODE_FUNCTION */
    { JS_ATOM_Function, js_bound_function_finalizer, js_bound_function_mark }, /* JS_CLASS_BOUND_FUNCTION */
    { JS_ATOM_Function, js_c_function_data_finalizer, js_c_function_data_mark }, /* JS_CLASS_C_FUNCTION_DATA */
    { JS_ATOM_GeneratorFunction, js_bytecode_function_finalizer, js_bytecode_function_mark },  /* JS_CLASS_GENERATOR_FUNCTION */
    { JS_ATOM_ForInIterator, js_for_in_iterator_finalizer, js_for_in_iterator_mark },      /* JS_CLASS_FOR_IN_ITERATOR */
    { JS_ATOM_RegExp, js_regexp_finalizer, NULL },                              /* JS_CLASS_REGEXP */
    { JS_ATOM_ArrayBuffer, js_array_buffer_finalizer, NULL },                   /* JS_CLASS_ARRAY_BUFFER */
    { JS_ATOM_SharedArrayBuffer, js_array_buffer_finalizer, NULL },             /* JS_CLASS_SHARED_ARRAY_BUFFER */
    { JS_ATOM_Uint8ClampedArray, js_typed_array_finalizer, js_typed_array_mark }, /* JS_CLASS_UINT8C_ARRAY */
    { JS_ATOM_Int8Array, js_typed_array_finalizer, js_typed_array_mark },       /* JS_CLASS_INT8_ARRAY */
    { JS_ATOM_Uint8Array, js_typed_array_finalizer, js_typed_array_mark },      /* JS_CLASS_UINT8_ARRAY */
    { JS_ATOM_Int16Array, js_typed_array_finalizer, js_typed_array_mark },      /* JS_CLASS_INT16_ARRAY */
    { JS_ATOM_Uint16Array, js_typed_array_finalizer, js_typed_array_mark },     /* JS_CLASS_UINT16_ARRAY */
    { JS_ATOM_Int32Array, js_typed_array_finalizer, js_typed_array_mark },      /* JS_CLASS_INT32_ARRAY */
    { JS_ATOM_Uint32Array, js_typed_array_finalizer, js_typed_array_mark },     /* JS_CLASS_UINT32_ARRAY */
#ifdef CONFIG_BIGNUM
    { JS_ATOM_BigInt64Array, js_typed_array_finalizer, js_typed_array_mark },   /* JS_CLASS_BIG_INT64_ARRAY */
    { JS_ATOM_BigUint64Array, js_typed_array_finalizer, js_typed_array_mark },  /* JS_CLASS_BIG_UINT64_ARRAY */
#endif
    { JS_ATOM_Float32Array, js_typed_array_finalizer, js_typed_array_mark },    /* JS_CLASS_FLOAT32_ARRAY */
    { JS_ATOM_Float64Array, js_typed_array_finalizer, js_typed_array_mark },    /* JS_CLASS_FLOAT64_ARRAY */
    { JS_ATOM_DataView, js_typed_array_finalizer, js_typed_array_mark },        /* JS_CLASS_DATAVIEW */
#ifdef CONFIG_BIGNUM
    { JS_ATOM_BigInt, js_object_data_finalizer, js_object_data_mark },      /* JS_CLASS_BIG_INT */
    { JS_ATOM_BigFloat, js_object_data_finalizer, js_object_data_mark },    /* JS_CLASS_BIG_FLOAT */
    { JS_ATOM_BigFloatEnv, js_float_env_finalizer, NULL },      /* JS_CLASS_FLOAT_ENV */
    { JS_ATOM_BigDecimal, js_object_data_finalizer, js_object_data_mark },    /* JS_CLASS_BIG_DECIMAL */
    { JS_ATOM_OperatorSet, js_operator_set_finalizer, js_operator_set_mark },    /* JS_CLASS_OPERATOR_SET */
#endif
    { JS_ATOM_Map, js_map_finalizer, js_map_mark },             /* JS_CLASS_MAP */
    { JS_ATOM_Set, js_map_finalizer, js_map_mark },             /* JS_CLASS_SET */
    { JS_ATOM_WeakMap, js_map_finalizer, js_map_mark },         /* JS_CLASS_WEAKMAP */
    { JS_ATOM_WeakSet, js_map_finalizer, js_map_mark },         /* JS_CLASS_WEAKSET */
    { JS_ATOM_Map_Iterator, js_map_iterator_finalizer, js_map_iterator_mark }, /* JS_CLASS_MAP_ITERATOR */
    { JS_ATOM_Set_Iterator, js_map_iterator_finalizer, js_map_iterator_mark }, /* JS_CLASS_SET_ITERATOR */
    { JS_ATOM_Array_Iterator, js_array_iterator_finalizer, js_array_iterator_mark }, /* JS_CLASS_ARRAY_ITERATOR */
    { JS_ATOM_String_Iterator, js_array_iterator_finalizer, js_array_iterator_mark }, /* JS_CLASS_STRING_ITERATOR */
    { JS_ATOM_RegExp_String_Iterator, js_regexp_string_iterator_finalizer, js_regexp_string_iterator_mark }, /* JS_CLASS_REGEXP_STRING_ITERATOR */
    { JS_ATOM_Generator, js_generator_finalizer, js_generator_mark }, /* JS_CLASS_GENERATOR */
};

static int init_class_range(JSRuntime *rt, JSClassShortDef const *tab,
                            int start, int count)
{
    JSClassDef cm_s, *cm = &cm_s;
    int i, class_id;

    for(i = 0; i < count; i++) {
        class_id = i + start;
        memset(cm, 0, sizeof(*cm));
        cm->finalizer = tab[i].finalizer;
        cm->gc_mark = tab[i].gc_mark;
        if (JS_NewClass1(rt, class_id, cm, tab[i].class_name) < 0)
            return -1;
    }
    return 0;
}

#ifdef CONFIG_BIGNUM
static JSValue JS_ThrowUnsupportedOperation(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "unsupported operation");
}

static JSValue invalid_to_string(JSContext *ctx, JSValueConst val)
{
    return JS_ThrowUnsupportedOperation(ctx);
}

static JSValue invalid_from_string(JSContext *ctx, const char *buf,
                                   int radix, int flags, slimb_t *pexponent)
{
    return JS_NAN;
}

static int invalid_unary_arith(JSContext *ctx,
                               JSValue *pres, OPCodeEnum op, JSValue op1)
{
    JS_FreeValue(ctx, op1);
    JS_ThrowUnsupportedOperation(ctx);
    return -1;
}

static int invalid_binary_arith(JSContext *ctx, OPCodeEnum op,
                                JSValue *pres, JSValue op1, JSValue op2)
{
    JS_FreeValue(ctx, op1);
    JS_FreeValue(ctx, op2);
    JS_ThrowUnsupportedOperation(ctx);
    return -1;
}

static JSValue invalid_mul_pow10_to_float64(JSContext *ctx, const bf_t *a,
                                            int64_t exponent)
{
    return JS_ThrowUnsupportedOperation(ctx);
}

static int invalid_mul_pow10(JSContext *ctx, JSValue *sp)
{
    JS_ThrowUnsupportedOperation(ctx);
    return -1;
}

static void set_dummy_numeric_ops(JSNumericOperations *ops)
{
    ops->to_string = invalid_to_string;
    ops->from_string = invalid_from_string;
    ops->unary_arith = invalid_unary_arith;
    ops->binary_arith = invalid_binary_arith;
    ops->mul_pow10_to_float64 = invalid_mul_pow10_to_float64;
    ops->mul_pow10 = invalid_mul_pow10;
}

#endif /* CONFIG_BIGNUM */

#if !defined(CONFIG_STACK_CHECK)
/* no stack limitation */
static inline uintptr_t js_get_stack_pointer(void)
{
    return 0;
}

static inline BOOL js_check_stack_overflow(JSRuntime *rt, size_t alloca_size)
{
    return FALSE;
}
#else
/* Note: OS and CPU dependent */
static inline uintptr_t js_get_stack_pointer(void)
{
    return (uintptr_t)__builtin_frame_address(0);
}

static inline BOOL js_check_stack_overflow(JSRuntime *rt, size_t alloca_size)
{
    uintptr_t sp;
    sp = js_get_stack_pointer() - alloca_size;
    return unlikely(sp < rt->stack_limit);
}
#endif

JSRuntime *JS_NewRuntime2(const JSMallocFunctions *mf, void *opaque)
{
    JSRuntime *rt;
    JSMallocState ms;

    memset(&ms, 0, sizeof(ms));
    ms.opaque = opaque;
    ms.malloc_limit = -1;

    rt = mf->js_malloc(&ms, sizeof(JSRuntime));
    if (!rt)
        return NULL;
    memset(rt, 0, sizeof(*rt));
    rt->mf = *mf;
    if (!rt->mf.js_malloc_usable_size) {
        /* use dummy function if none provided */
        rt->mf.js_malloc_usable_size = js_malloc_usable_size_unknown;
    }
    rt->malloc_state = ms;
    rt->malloc_gc_threshold = 256 * 1024;

#ifdef CONFIG_BIGNUM
    bf_context_init(&rt->bf_ctx, js_bf_realloc, rt);
    set_dummy_numeric_ops(&rt->bigint_ops);
    set_dummy_numeric_ops(&rt->bigfloat_ops);
    set_dummy_numeric_ops(&rt->bigdecimal_ops);
#endif

    init_list_head(&rt->context_list);
    init_list_head(&rt->gc_obj_list);
    init_list_head(&rt->gc_zero_ref_count_list);
    rt->gc_phase = JS_GC_PHASE_NONE;
    
#ifdef DUMP_LEAKS
    init_list_head(&rt->string_list);
#endif
    init_list_head(&rt->job_list);

    if (JS_InitAtoms(rt))
        goto fail;

    /* create the object, array and function classes */
    if (init_class_range(rt, js_std_class_def, JS_CLASS_OBJECT,
                         countof(js_std_class_def)) < 0)
        goto fail;
    rt->class_array[JS_CLASS_ARGUMENTS].exotic = &js_arguments_exotic_methods;
    rt->class_array[JS_CLASS_STRING].exotic = &js_string_exotic_methods;
    rt->class_array[JS_CLASS_MODULE_NS].exotic = &js_module_ns_exotic_methods;

    rt->class_array[JS_CLASS_C_FUNCTION].call = js_call_c_function;
    rt->class_array[JS_CLASS_C_FUNCTION_DATA].call = js_c_function_data_call;
    rt->class_array[JS_CLASS_BOUND_FUNCTION].call = js_call_bound_function;
    rt->class_array[JS_CLASS_GENERATOR_FUNCTION].call = js_generator_function_call;
    if (init_shape_hash(rt))
        goto fail;

    rt->stack_size = JS_DEFAULT_STACK_SIZE;
    JS_UpdateStackTop(rt);

    rt->current_exception = JS_NULL;

    return rt;
 fail:
    JS_FreeRuntime(rt);
    return NULL;
}

void *JS_GetRuntimeOpaque(JSRuntime *rt)
{
    return rt->user_opaque;
}

void JS_SetRuntimeOpaque(JSRuntime *rt, void *opaque)
{
    rt->user_opaque = opaque;
}

/* default memory allocation functions with memory limitation */
static inline size_t js_def_malloc_usable_size(void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize(ptr);
#elif defined(EMSCRIPTEN)
    return 0;
#elif defined(__linux__)
    return malloc_usable_size(ptr);
#else
    /* change this to `return 0;` if compilation fails */
    return malloc_usable_size(ptr);
#endif
}

static void *js_def_malloc(JSMallocState *s, size_t size)
{
    void *ptr;

    /* Do not allocate zero bytes: behavior is platform dependent */
    assert(size != 0);

    if (unlikely(s->malloc_size + size > s->malloc_limit))
        return NULL;

    ptr = malloc(size);
    if (!ptr)
        return NULL;

    s->malloc_count++;
    s->malloc_size += js_def_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    return ptr;
}

static void js_def_free(JSMallocState *s, void *ptr)
{
    if (!ptr)
        return;

    s->malloc_count--;
    s->malloc_size -= js_def_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
    free(ptr);
}

static void *js_def_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;

    if (!ptr) {
        if (size == 0)
            return NULL;
        return js_def_malloc(s, size);
    }
    old_size = js_def_malloc_usable_size(ptr);
    if (size == 0) {
        s->malloc_count--;
        s->malloc_size -= old_size + MALLOC_OVERHEAD;
        free(ptr);
        return NULL;
    }
    if (s->malloc_size + size - old_size > s->malloc_limit)
        return NULL;

    ptr = realloc(ptr, size);
    if (!ptr)
        return NULL;

    s->malloc_size += js_def_malloc_usable_size(ptr) - old_size;
    return ptr;
}

static const JSMallocFunctions def_malloc_funcs = {
    js_def_malloc,
    js_def_free,
    js_def_realloc,
#if defined(__APPLE__)
    malloc_size,
#elif defined(_WIN32)
    (size_t (*)(const void *))_msize,
#elif defined(EMSCRIPTEN)
    NULL,
#elif defined(__linux__)
    (size_t (*)(const void *))malloc_usable_size,
#else
    /* change this to `NULL,` if compilation fails */
    malloc_usable_size,
#endif
};

JSRuntime *JS_NewRuntime(void)
{
    return JS_NewRuntime2(&def_malloc_funcs, NULL);
}

void JS_SetMemoryLimit(JSRuntime *rt, size_t limit)
{
    rt->malloc_state.malloc_limit = limit;
}

/* use -1 to disable automatic GC */
void JS_SetGCThreshold(JSRuntime *rt, size_t gc_threshold)
{
    rt->malloc_gc_threshold = gc_threshold;
}

#define malloc(s) malloc_is_forbidden(s)
#define free(p) free_is_forbidden(p)
#define realloc(p,s) realloc_is_forbidden(p,s)

void JS_SetInterruptHandler(JSRuntime *rt, JSInterruptHandler *cb, void *opaque)
{
    rt->interrupt_handler = cb;
    rt->interrupt_opaque = opaque;
}

void JS_SetCanBlock(JSRuntime *rt, BOOL can_block)
{
    rt->can_block = can_block;
}

void JS_SetSharedArrayBufferFunctions(JSRuntime *rt,
                                      const JSSharedArrayBufferFunctions *sf)
{
    rt->sab_funcs = *sf;
}

/* return 0 if OK, < 0 if exception */
int JS_EnqueueJob(JSContext *ctx, JSJobFunc *job_func,
                  int argc, JSValueConst *argv)
{
    JSRuntime *rt = ctx->rt;
    JSJobEntry *e;
    int i;

    e = js_malloc(ctx, sizeof(*e) + argc * sizeof(JSValue));
    if (!e)
        return -1;
    e->ctx = ctx;
    e->job_func = job_func;
    e->argc = argc;
    for(i = 0; i < argc; i++) {
        e->argv[i] = JS_DupValue(ctx, argv[i]);
    }
    list_add_tail(&e->link, &rt->job_list);
    return 0;
}

BOOL JS_IsJobPending(JSRuntime *rt)
{
    return !list_empty(&rt->job_list);
}

/* return < 0 if exception, 0 if no job pending, 1 if a job was
   executed successfully. the context of the job is stored in '*pctx' */
int JS_ExecutePendingJob(JSRuntime *rt, JSContext **pctx)
{
    JSContext *ctx;
    JSJobEntry *e;
    JSValue res;
    int i, ret;

    if (list_empty(&rt->job_list)) {
        *pctx = NULL;
        return 0;
    }

    /* get the first pending job and execute it */
    e = list_entry(rt->job_list.next, JSJobEntry, link);
    list_del(&e->link);
    ctx = e->ctx;
    res = e->job_func(e->ctx, e->argc, (JSValueConst *)e->argv);
    for(i = 0; i < e->argc; i++)
        JS_FreeValue(ctx, e->argv[i]);
    if (JS_IsException(res))
        ret = -1;
    else
        ret = 1;
    JS_FreeValue(ctx, res);
    js_free(ctx, e);
    *pctx = ctx;
    return ret;
}

static inline uint32_t atom_get_free(const JSAtomStruct *p)
{
    return (uintptr_t)p >> 1;
}

static inline BOOL atom_is_free(const JSAtomStruct *p)
{
    return (uintptr_t)p & 1;
}

static inline JSAtomStruct *atom_set_free(uint32_t v)
{
    return (JSAtomStruct *)(((uintptr_t)v << 1) | 1);
}

/* Note: the string contents are uninitialized */
static JSString *js_alloc_string_rt(JSRuntime *rt, int max_len, int is_wide_char)
{
    JSString *str;
    str = js_malloc_rt(rt, sizeof(JSString) + (max_len << is_wide_char) + 1 - is_wide_char);
    if (unlikely(!str))
        return NULL;
    str->header.ref_count = 1;
    str->is_wide_char = is_wide_char;
    str->len = max_len;
    str->atom_type = 0;
    str->hash = 0;          /* optional but costless */
    str->hash_next = 0;     /* optional */
#ifdef DUMP_LEAKS
    list_add_tail(&str->link, &rt->string_list);
#endif
    return str;
}

static JSString *js_alloc_string(JSContext *ctx, int max_len, int is_wide_char)
{
    JSString *p;
    p = js_alloc_string_rt(ctx->rt, max_len, is_wide_char);
    if (unlikely(!p)) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    return p;
}

/* same as JS_FreeValueRT() but faster */
static inline void js_free_string(JSRuntime *rt, JSString *str)
{
    if (--str->header.ref_count <= 0) {
        if (str->atom_type) {
            JS_FreeAtomStruct(rt, str);
        } else {
#ifdef DUMP_LEAKS
            list_del(&str->link);
#endif
            js_free_rt(rt, str);
        }
    }
}

void JS_SetRuntimeInfo(JSRuntime *rt, const char *s)
{
    if (rt)
        rt->rt_info = s;
}

void JS_FreeRuntime(JSRuntime *rt)
{
    struct list_head *el, *el1;
    int i;

    JS_FreeValueRT(rt, rt->current_exception);

    list_for_each_safe(el, el1, &rt->job_list) {
        JSJobEntry *e = list_entry(el, JSJobEntry, link);
        for(i = 0; i < e->argc; i++)
            JS_FreeValueRT(rt, e->argv[i]);
        js_free_rt(rt, e);
    }
    init_list_head(&rt->job_list);

    JS_RunGC(rt);

#ifdef DUMP_LEAKS
    /* leaking objects */
    {
        BOOL header_done;
        JSGCObjectHeader *p;
        int count;

        /* remove the internal refcounts to display only the object
           referenced externally */
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            p->mark = 0;
        }
        gc_decref(rt);

        header_done = FALSE;
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            if (p->ref_count != 0) {
                if (!header_done) {
                    printf("Object leaks:\n");
                    JS_DumpObjectHeader(rt);
                    header_done = TRUE;
                }
                JS_DumpGCObject(rt, p);
            }
        }

        count = 0;
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            if (p->ref_count == 0) {
                count++;
            }
        }
        if (count != 0)
            printf("Secondary object leaks: %d\n", count);
    }
#endif
    assert(list_empty(&rt->gc_obj_list));

    /* free the classes */
    for(i = 0; i < rt->class_count; i++) {
        JSClass *cl = &rt->class_array[i];
        if (cl->class_id != 0) {
            JS_FreeAtomRT(rt, cl->class_name);
        }
    }
    js_free_rt(rt, rt->class_array);

#ifdef CONFIG_BIGNUM
    bf_context_end(&rt->bf_ctx);
#endif

#ifdef DUMP_LEAKS
    /* only the atoms defined in JS_InitAtoms() should be left */
    {
        BOOL header_done = FALSE;

        for(i = 0; i < rt->atom_size; i++) {
            JSAtomStruct *p = rt->atom_array[i];
            if (!atom_is_free(p) /* && p->str*/) {
                if (i >= JS_ATOM_END || p->header.ref_count != 1) {
                    if (!header_done) {
                        header_done = TRUE;
                        if (rt->rt_info) {
                            printf("%s:1: atom leakage:", rt->rt_info);
                        } else {
                            printf("Atom leaks:\n"
                                   "    %6s %6s %s\n",
                                   "ID", "REFCNT", "NAME");
                        }
                    }
                    if (rt->rt_info) {
                        printf(" ");
                    } else {
                        printf("    %6u %6u ", i, p->header.ref_count);
                    }
                    switch (p->atom_type) {
                    case JS_ATOM_TYPE_STRING:
                        JS_DumpString(rt, p);
                        break;
                    case JS_ATOM_TYPE_GLOBAL_SYMBOL:
                        printf("Symbol.for(");
                        JS_DumpString(rt, p);
                        printf(")");
                        break;
                    case JS_ATOM_TYPE_SYMBOL:
                        if (p->hash == JS_ATOM_HASH_SYMBOL) {
                            printf("Symbol(");
                            JS_DumpString(rt, p);
                            printf(")");
                        } else {
                            printf("Private(");
                            JS_DumpString(rt, p);
                            printf(")");
                        }
                        break;
                    }
                    if (rt->rt_info) {
                        printf(":%u", p->header.ref_count);
                    } else {
                        printf("\n");
                    }
                }
            }
        }
        if (rt->rt_info && header_done)
            printf("\n");
    }
#endif

    /* free the atoms */
    for(i = 0; i < rt->atom_size; i++) {
        JSAtomStruct *p = rt->atom_array[i];
        if (!atom_is_free(p)) {
#ifdef DUMP_LEAKS
            list_del(&p->link);
#endif
            js_free_rt(rt, p);
        }
    }
    js_free_rt(rt, rt->atom_array);
    js_free_rt(rt, rt->atom_hash);
    js_free_rt(rt, rt->shape_hash);
#ifdef DUMP_LEAKS
    if (!list_empty(&rt->string_list)) {
        if (rt->rt_info) {
            printf("%s:1: string leakage:", rt->rt_info);
        } else {
            printf("String leaks:\n"
                   "    %6s %s\n",
                   "REFCNT", "VALUE");
        }
        list_for_each_safe(el, el1, &rt->string_list) {
            JSString *str = list_entry(el, JSString, link);
            if (rt->rt_info) {
                printf(" ");
            } else {
                printf("    %6u ", str->header.ref_count);
            }
            JS_DumpString(rt, str);
            if (rt->rt_info) {
                printf(":%u", str->header.ref_count);
            } else {
                printf("\n");
            }
            list_del(&str->link);
            js_free_rt(rt, str);
        }
        if (rt->rt_info)
            printf("\n");
    }
    {
        JSMallocState *s = &rt->malloc_state;
        if (s->malloc_count > 1) {
            if (rt->rt_info)
                printf("%s:1: ", rt->rt_info);
            printf("Memory leak: %"PRIu64" bytes lost in %"PRIu64" block%s\n",
                   (uint64_t)(s->malloc_size - sizeof(JSRuntime)),
                   (uint64_t)(s->malloc_count - 1), &"s"[s->malloc_count == 2]);
        }
    }
#endif

    {
        JSMallocState ms = rt->malloc_state;
        rt->mf.js_free(&ms, rt);
    }
}

JSContext *JS_NewContextRaw(JSRuntime *rt)
{
    JSContext *ctx;
    int i;

    ctx = js_mallocz_rt(rt, sizeof(JSContext));
    if (!ctx)
        return NULL;
    ctx->header.ref_count = 1;
    add_gc_object(rt, &ctx->header, JS_GC_OBJ_TYPE_JS_CONTEXT);

    ctx->class_proto = js_malloc_rt(rt, sizeof(ctx->class_proto[0]) *
                                    rt->class_count);
    if (!ctx->class_proto) {
        js_free_rt(rt, ctx);
        return NULL;
    }
    ctx->rt = rt;
    list_add_tail(&ctx->link, &rt->context_list);
#ifdef CONFIG_BIGNUM
    ctx->bf_ctx = &rt->bf_ctx;
    ctx->fp_env.prec = 113;
    ctx->fp_env.flags = bf_set_exp_bits(15) | BF_RNDN | BF_FLAG_SUBNORMAL;
#endif
    for(i = 0; i < rt->class_count; i++)
        ctx->class_proto[i] = JS_NULL;
    ctx->array_ctor = JS_NULL;
    ctx->regexp_ctor = JS_NULL;
    ctx->promise_ctor = JS_NULL;
    init_list_head(&ctx->loaded_modules);

    JS_AddIntrinsicBasicObjects(ctx);
    return ctx;
}

JSContext *JS_NewContext(JSRuntime *rt)
{
    JSContext *ctx;

    ctx = JS_NewContextRaw(rt);
    if (!ctx)
        return NULL;

    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicDate(ctx);
    JS_AddIntrinsicEval(ctx);
    JS_AddIntrinsicStringNormalize(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicProxy(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicPromise(ctx);
#ifdef CONFIG_BIGNUM
    JS_AddIntrinsicBigInt(ctx);
#endif
    return ctx;
}

void *JS_GetContextOpaque(JSContext *ctx)
{
    return ctx->user_opaque;
}

void JS_SetContextOpaque(JSContext *ctx, void *opaque)
{
    ctx->user_opaque = opaque;
}

/* set the new value and free the old value after (freeing the value
   can reallocate the object data) */
static inline void set_value(JSContext *ctx, JSValue *pval, JSValue new_val)
{
    JSValue old_val;
    old_val = *pval;
    *pval = new_val;
    JS_FreeValue(ctx, old_val);
}

void JS_SetClassProto(JSContext *ctx, JSClassID class_id, JSValue obj)
{
    JSRuntime *rt = ctx->rt;
    assert(class_id < rt->class_count);
    set_value(ctx, &ctx->class_proto[class_id], obj);
}

JSValue JS_GetClassProto(JSContext *ctx, JSClassID class_id)
{
    JSRuntime *rt = ctx->rt;
    assert(class_id < rt->class_count);
    return JS_DupValue(ctx, ctx->class_proto[class_id]);
}

typedef enum JSFreeModuleEnum {
    JS_FREE_MODULE_ALL,
    JS_FREE_MODULE_NOT_RESOLVED,
    JS_FREE_MODULE_NOT_EVALUATED,
} JSFreeModuleEnum;

/* XXX: would be more efficient with separate module lists */
static void js_free_modules(JSContext *ctx, JSFreeModuleEnum flag)
{
    struct list_head *el, *el1;
    list_for_each_safe(el, el1, &ctx->loaded_modules) {
        JSModuleDef *m = list_entry(el, JSModuleDef, link);
        if (flag == JS_FREE_MODULE_ALL ||
            (flag == JS_FREE_MODULE_NOT_RESOLVED && !m->resolved) ||
            (flag == JS_FREE_MODULE_NOT_EVALUATED && !m->evaluated)) {
            js_free_module_def(ctx, m);
        }
    }
}

JSContext *JS_DupContext(JSContext *ctx)
{
    ctx->header.ref_count++;
    return ctx;
}

/* used by the GC */
static void JS_MarkContext(JSRuntime *rt, JSContext *ctx,
                           JS_MarkFunc *mark_func)
{
    int i;
    struct list_head *el;

    /* modules are not seen by the GC, so we directly mark the objects
       referenced by each module */
    list_for_each(el, &ctx->loaded_modules) {
        JSModuleDef *m = list_entry(el, JSModuleDef, link);
        js_mark_module_def(rt, m, mark_func);
    }

    JS_MarkValue(rt, ctx->global_obj, mark_func);
    JS_MarkValue(rt, ctx->global_var_obj, mark_func);

    JS_MarkValue(rt, ctx->throw_type_error, mark_func);
    JS_MarkValue(rt, ctx->eval_obj, mark_func);

    JS_MarkValue(rt, ctx->array_proto_values, mark_func);
    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        JS_MarkValue(rt, ctx->native_error_proto[i], mark_func);
    }
    for(i = 0; i < rt->class_count; i++) {
        JS_MarkValue(rt, ctx->class_proto[i], mark_func);
    }
    JS_MarkValue(rt, ctx->iterator_proto, mark_func);
    JS_MarkValue(rt, ctx->async_iterator_proto, mark_func);
    JS_MarkValue(rt, ctx->promise_ctor, mark_func);
    JS_MarkValue(rt, ctx->array_ctor, mark_func);
    JS_MarkValue(rt, ctx->regexp_ctor, mark_func);
    JS_MarkValue(rt, ctx->function_ctor, mark_func);
    JS_MarkValue(rt, ctx->function_proto, mark_func);

    if (ctx->array_shape)
        mark_func(rt, &ctx->array_shape->header);
}

void JS_FreeContext(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    int i;

    if (--ctx->header.ref_count > 0)
        return;
    assert(ctx->header.ref_count == 0);
    
#ifdef DUMP_ATOMS
    JS_DumpAtoms(ctx->rt);
#endif
#ifdef DUMP_SHAPES
    JS_DumpShapes(ctx->rt);
#endif
#ifdef DUMP_OBJECTS
    {
        struct list_head *el;
        JSGCObjectHeader *p;
        printf("JSObjects: {\n");
        JS_DumpObjectHeader(ctx->rt);
        list_for_each(el, &rt->gc_obj_list) {
            p = list_entry(el, JSGCObjectHeader, link);
            JS_DumpGCObject(rt, p);
        }
        printf("}\n");
    }
#endif
#ifdef DUMP_MEM
    {
        JSMemoryUsage stats;
        JS_ComputeMemoryUsage(rt, &stats);
        JS_DumpMemoryUsage(stdout, &stats, rt);
    }
#endif

    js_free_modules(ctx, JS_FREE_MODULE_ALL);

    JS_FreeValue(ctx, ctx->global_obj);
    JS_FreeValue(ctx, ctx->global_var_obj);

    JS_FreeValue(ctx, ctx->throw_type_error);
    JS_FreeValue(ctx, ctx->eval_obj);

    JS_FreeValue(ctx, ctx->array_proto_values);
    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        JS_FreeValue(ctx, ctx->native_error_proto[i]);
    }
    for(i = 0; i < rt->class_count; i++) {
        JS_FreeValue(ctx, ctx->class_proto[i]);
    }
    js_free_rt(rt, ctx->class_proto);
    JS_FreeValue(ctx, ctx->iterator_proto);
    JS_FreeValue(ctx, ctx->async_iterator_proto);
    JS_FreeValue(ctx, ctx->promise_ctor);
    JS_FreeValue(ctx, ctx->array_ctor);
    JS_FreeValue(ctx, ctx->regexp_ctor);
    JS_FreeValue(ctx, ctx->function_ctor);
    JS_FreeValue(ctx, ctx->function_proto);

    js_free_shape_null(ctx->rt, ctx->array_shape);

    list_del(&ctx->link);
    remove_gc_object(&ctx->header);
    js_free_rt(ctx->rt, ctx);
}

JSRuntime *JS_GetRuntime(JSContext *ctx)
{
    return ctx->rt;
}

static void update_stack_limit(JSRuntime *rt)
{
    if (rt->stack_size == 0) {
        rt->stack_limit = 0; /* no limit */
    } else {
        rt->stack_limit = rt->stack_top - rt->stack_size;
    }
}

void JS_SetMaxStackSize(JSRuntime *rt, size_t stack_size)
{
    rt->stack_size = stack_size;
    update_stack_limit(rt);
}

void JS_UpdateStackTop(JSRuntime *rt)
{
    rt->stack_top = js_get_stack_pointer();
    update_stack_limit(rt);
}

static inline BOOL is_strict_mode(JSContext *ctx)
{
    JSStackFrame *sf = ctx->rt->current_stack_frame;
    return (sf && (sf->js_mode & JS_MODE_STRICT));
}

#ifdef CONFIG_BIGNUM
static inline BOOL is_math_mode(JSContext *ctx)
{
    JSStackFrame *sf = ctx->rt->current_stack_frame;
    return (sf && (sf->js_mode & JS_MODE_MATH));
}
#endif

/* JSAtom support */

#include "quickjs-jsatom.c"

/* JSClass support */

#include "quickjs-jsclass.c"

/* JS parser */

#include "quickjs-jsparser.c"

/* JSON */

static int json_parse_expect(JSParseState *s, int tok)
{
    if (s->token.val != tok) {
        /* XXX: dump token correctly in all cases */
        return js_parse_error(s, "expecting '%c'", tok);
    }
    return json_next_token(s);
}

static JSValue json_parse_value(JSParseState *s)
{
    JSContext *ctx = s->ctx;
    JSValue val = JS_NULL;
    int ret;

    switch(s->token.val) {
    case '{':
        {
            JSValue prop_val;
            JSAtom prop_name;
            
            if (json_next_token(s))
                goto fail;
            val = JS_NewObject(ctx);
            if (JS_IsException(val))
                goto fail;
            if (s->token.val != '}') {
                for(;;) {
                    if (s->token.val == TOK_STRING) {
                        prop_name = JS_ValueToAtom(ctx, s->token.u.str.str);
                        if (prop_name == JS_ATOM_NULL)
                            goto fail;
                    } else if (s->ext_json && s->token.val == TOK_IDENT) {
                        prop_name = JS_DupAtom(ctx, s->token.u.ident.atom);
                    } else {
                        js_parse_error(s, "expecting property name");
                        goto fail;
                    }
                    if (json_next_token(s))
                        goto fail1;
                    if (json_parse_expect(s, ':'))
                        goto fail1;
                    prop_val = json_parse_value(s);
                    if (JS_IsException(prop_val)) {
                    fail1:
                        JS_FreeAtom(ctx, prop_name);
                        goto fail;
                    }
                    ret = JS_DefinePropertyValue(ctx, val, prop_name,
                                                 prop_val, JS_PROP_C_W_E);
                    JS_FreeAtom(ctx, prop_name);
                    if (ret < 0)
                        goto fail;

                    if (s->token.val != ',')
                        break;
                    if (json_next_token(s))
                        goto fail;
                    if (s->ext_json && s->token.val == '}')
                        break;
                }
            }
            if (json_parse_expect(s, '}'))
                goto fail;
        }
        break;
    case '[':
        {
            JSValue el;
            uint32_t idx;

            if (json_next_token(s))
                goto fail;
            val = JS_NewArray(ctx);
            if (JS_IsException(val))
                goto fail;
            if (s->token.val != ']') {
                idx = 0;
                for(;;) {
                    el = json_parse_value(s);
                    if (JS_IsException(el))
                        goto fail;
                    ret = JS_DefinePropertyValueUint32(ctx, val, idx, el, JS_PROP_C_W_E);
                    if (ret < 0)
                        goto fail;
                    if (s->token.val != ',')
                        break;
                    if (json_next_token(s))
                        goto fail;
                    idx++;
                    if (s->ext_json && s->token.val == ']')
                        break;
                }
            }
            if (json_parse_expect(s, ']'))
                goto fail;
        }
        break;
    case TOK_STRING:
        val = JS_DupValue(ctx, s->token.u.str.str);
        if (json_next_token(s))
            goto fail;
        break;
    case TOK_NUMBER:
        val = s->token.u.num.val;
        if (json_next_token(s))
            goto fail;
        break;
    case TOK_IDENT:
        if (s->token.u.ident.atom == JS_ATOM_false ||
            s->token.u.ident.atom == JS_ATOM_true) {
            val = JS_NewBool(ctx, (s->token.u.ident.atom == JS_ATOM_true));
        } else if (s->token.u.ident.atom == JS_ATOM_null) {
            val = JS_NULL;
        } else {
            goto def_token;
        }
        if (json_next_token(s))
            goto fail;
        break;
    default:
    def_token:
        if (s->token.val == TOK_EOF) {
            js_parse_error(s, "unexpected end of input");
        } else {
            js_parse_error(s, "unexpected token: '%.*s'",
                           (int)(s->buf_ptr - s->token.ptr), s->token.ptr);
        }
        goto fail;
    }
    return val;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

JSValue JS_ParseJSON2(JSContext *ctx, const char *buf, size_t buf_len,
                      const char *filename, int flags)
{
    JSParseState s1, *s = &s1;
    JSValue val = JS_UNDEFINED;

    js_parse_init(ctx, s, buf, buf_len, filename);
    s->ext_json = ((flags & JS_PARSE_JSON_EXT) != 0);
    if (json_next_token(s))
        goto fail;
    val = json_parse_value(s);
    if (JS_IsException(val))
        goto fail;
    if (s->token.val != TOK_EOF) {
        if (js_parse_error(s, "unexpected data at the end"))
            goto fail;
    }
    return val;
 fail:
    JS_FreeValue(ctx, val);
    free_token(s, &s->token);
    return JS_EXCEPTION;
}

JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len,
                     const char *filename)
{
    return JS_ParseJSON2(ctx, buf, buf_len, filename, 0); 
}

static JSValue internalize_json_property(JSContext *ctx, JSValueConst holder,
                                         JSAtom name, JSValueConst reviver)
{
    JSValue val, new_el, name_val, res;
    JSValueConst args[2];
    int ret, is_array;
    uint32_t i, len = 0;
    JSAtom prop;
    JSPropertyEnum *atoms = NULL;

    if (js_check_stack_overflow(ctx->rt, 0)) {
        return JS_ThrowStackOverflow(ctx);
    }

    val = JS_GetProperty(ctx, holder, name);
    if (JS_IsException(val))
        return val;
    if (JS_IsObject(val)) {
        is_array = JS_IsArray(ctx, val);
        if (is_array < 0)
            goto fail;
        if (is_array) {
            if (js_get_length32(ctx, &len, val))
                goto fail;
        } else {
            ret = JS_GetOwnPropertyNamesInternal(ctx, &atoms, &len, JS_VALUE_GET_OBJ(val), JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK);
            if (ret < 0)
                goto fail;
        }
        for(i = 0; i < len; i++) {
            if (is_array) {
                prop = JS_NewAtomUInt32(ctx, i);
                if (prop == JS_ATOM_NULL)
                    goto fail;
            } else {
                prop = JS_DupAtom(ctx, atoms[i].atom);
            }
            new_el = internalize_json_property(ctx, val, prop, reviver);
            if (JS_IsException(new_el)) {
                JS_FreeAtom(ctx, prop);
                goto fail;
            }
            if (JS_IsUndefined(new_el)) {
                ret = JS_DeleteProperty(ctx, val, prop, 0);
            } else {
                ret = JS_DefinePropertyValue(ctx, val, prop, new_el, JS_PROP_C_W_E);
            }
            JS_FreeAtom(ctx, prop);
            if (ret < 0)
                goto fail;
        }
    }
    js_free_prop_enum(ctx, atoms, len);
    atoms = NULL;
    name_val = JS_AtomToValue(ctx, name);
    if (JS_IsException(name_val))
        goto fail;
    args[0] = name_val;
    args[1] = val;
    res = JS_Call(ctx, reviver, holder, 2, args);
    JS_FreeValue(ctx, name_val);
    JS_FreeValue(ctx, val);
    return res;
 fail:
    js_free_prop_enum(ctx, atoms, len);
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_json_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue obj, root;
    JSValueConst reviver;
    const char *str;
    size_t len;

    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    obj = JS_ParseJSON(ctx, str, len, "<input>");
    JS_FreeCString(ctx, str);
    if (JS_IsException(obj))
        return obj;
    if (argc > 1 && JS_IsFunction(ctx, argv[1])) {
        reviver = argv[1];
        root = JS_NewObject(ctx);
        if (JS_IsException(root)) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (JS_DefinePropertyValue(ctx, root, JS_ATOM_empty_string, obj,
                                   JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, root);
            return JS_EXCEPTION;
        }
        obj = internalize_json_property(ctx, root, JS_ATOM_empty_string,
                                        reviver);
        JS_FreeValue(ctx, root);
    }
    return obj;
}

typedef struct JSONStringifyContext {
    JSValueConst replacer_func;
    JSValue stack;
    JSValue property_list;
    JSValue gap;
    JSValue empty;
    StringBuffer *b;
} JSONStringifyContext;

static JSValue JS_ToQuotedStringFree(JSContext *ctx, JSValue val) {
    JSValue r = JS_ToQuotedString(ctx, val);
    JS_FreeValue(ctx, val);
    return r;
}

static JSValue js_json_check(JSContext *ctx, JSONStringifyContext *jsc,
                             JSValueConst holder, JSValue val, JSValueConst key)
{
    JSValue v;
    JSValueConst args[2];

    if (JS_IsObject(val)
#ifdef CONFIG_BIGNUM
    ||  JS_IsBigInt(ctx, val)   /* XXX: probably useless */
#endif
        ) {
            JSValue f = JS_GetProperty(ctx, val, JS_ATOM_toJSON);
            if (JS_IsException(f))
                goto exception;
            if (JS_IsFunction(ctx, f)) {
                v = JS_CallFree(ctx, f, val, 1, &key);
                JS_FreeValue(ctx, val);
                val = v;
                if (JS_IsException(val))
                    goto exception;
            } else {
                JS_FreeValue(ctx, f);
            }
        }

    if (!JS_IsUndefined(jsc->replacer_func)) {
        args[0] = key;
        args[1] = val;
        v = JS_Call(ctx, jsc->replacer_func, holder, 2, args);
        JS_FreeValue(ctx, val);
        val = v;
        if (JS_IsException(val))
            goto exception;
    }

    switch (JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_OBJECT:
        if (JS_IsFunction(ctx, val))
            break;
    case JS_TAG_STRING:
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:
#ifdef CONFIG_BIGNUM
    case JS_TAG_BIG_FLOAT:
#endif
    case JS_TAG_BOOL:
    case JS_TAG_NULL:
#ifdef CONFIG_BIGNUM
    case JS_TAG_BIG_INT:
#endif
    case JS_TAG_EXCEPTION:
        return val;
    default:
        break;
    }
    JS_FreeValue(ctx, val);
    return JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static int js_json_to_str(JSContext *ctx, JSONStringifyContext *jsc,
                          JSValueConst holder, JSValue val,
                          JSValueConst indent)
{
    JSValue indent1, sep, sep1, tab, v, prop;
    JSObject *p;
    int64_t i, len;
    int cl, ret;
    BOOL has_content;
    
    indent1 = JS_UNDEFINED;
    sep = JS_UNDEFINED;
    sep1 = JS_UNDEFINED;
    tab = JS_UNDEFINED;
    prop = JS_UNDEFINED;

    switch (JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_OBJECT:
        p = JS_VALUE_GET_OBJ(val);
        cl = p->class_id;
        if (cl == JS_CLASS_STRING) {
            val = JS_ToStringFree(ctx, val);
            if (JS_IsException(val))
                goto exception;
            val = JS_ToQuotedStringFree(ctx, val);
            if (JS_IsException(val))
                goto exception;
            return string_buffer_concat_value_free(jsc->b, val);
        } else if (cl == JS_CLASS_NUMBER) {
            val = JS_ToNumberFree(ctx, val);
            if (JS_IsException(val))
                goto exception;
            return string_buffer_concat_value_free(jsc->b, val);
        } else if (cl == JS_CLASS_BOOLEAN) {
            ret = string_buffer_concat_value(jsc->b, p->u.object_data);
            JS_FreeValue(ctx, val);
            return ret;
        }
#ifdef CONFIG_BIGNUM
        else if (cl == JS_CLASS_BIG_FLOAT) {
            return string_buffer_concat_value_free(jsc->b, val);
        } else if (cl == JS_CLASS_BIG_INT) {
            JS_ThrowTypeError(ctx, "bigint are forbidden in JSON.stringify");
            goto exception;
        }
#endif
        v = js_array_includes(ctx, jsc->stack, 1, (JSValueConst *)&val);
        if (JS_IsException(v))
            goto exception;
        if (JS_ToBoolFree(ctx, v)) {
            JS_ThrowTypeError(ctx, "circular reference");
            goto exception;
        }
        indent1 = JS_ConcatString(ctx, JS_DupValue(ctx, indent), JS_DupValue(ctx, jsc->gap));
        if (JS_IsException(indent1))
            goto exception;
        if (!JS_IsEmptyString(jsc->gap)) {
            sep = JS_ConcatString3(ctx, "\n", JS_DupValue(ctx, indent1), "");
            if (JS_IsException(sep))
                goto exception;
            sep1 = JS_NewString(ctx, " ");
            if (JS_IsException(sep1))
                goto exception;
        } else {
            sep = JS_DupValue(ctx, jsc->empty);
            sep1 = JS_DupValue(ctx, jsc->empty);
        }
        v = js_array_push(ctx, jsc->stack, 1, (JSValueConst *)&val, 0);
        if (check_exception_free(ctx, v))
            goto exception;
        ret = JS_IsArray(ctx, val);
        if (ret < 0)
            goto exception;
        if (ret) {
            if (js_get_length64(ctx, &len, val))
                goto exception;
            string_buffer_putc8(jsc->b, '[');
            for(i = 0; i < len; i++) {
                if (i > 0)
                    string_buffer_putc8(jsc->b, ',');
                string_buffer_concat_value(jsc->b, sep);
                v = JS_GetPropertyInt64(ctx, val, i);
                if (JS_IsException(v))
                    goto exception;
                /* XXX: could do this string conversion only when needed */
                prop = JS_ToStringFree(ctx, JS_NewInt64(ctx, i));
                if (JS_IsException(prop))
                    goto exception;
                v = js_json_check(ctx, jsc, val, v, prop);
                JS_FreeValue(ctx, prop);
                prop = JS_UNDEFINED;
                if (JS_IsException(v))
                    goto exception;
                if (JS_IsUndefined(v))
                    v = JS_NULL;
                if (js_json_to_str(ctx, jsc, val, v, indent1))
                    goto exception;
            }
            if (len > 0 && !JS_IsEmptyString(jsc->gap)) {
                string_buffer_putc8(jsc->b, '\n');
                string_buffer_concat_value(jsc->b, indent);
            }
            string_buffer_putc8(jsc->b, ']');
        } else {
            if (!JS_IsUndefined(jsc->property_list))
                tab = JS_DupValue(ctx, jsc->property_list);
            else
                tab = js_object_keys(ctx, JS_UNDEFINED, 1, (JSValueConst *)&val, JS_ITERATOR_KIND_KEY);
            if (JS_IsException(tab))
                goto exception;
            if (js_get_length64(ctx, &len, tab))
                goto exception;
            string_buffer_putc8(jsc->b, '{');
            has_content = FALSE;
            for(i = 0; i < len; i++) {
                JS_FreeValue(ctx, prop);
                prop = JS_GetPropertyInt64(ctx, tab, i);
                if (JS_IsException(prop))
                    goto exception;
                v = JS_GetPropertyValue(ctx, val, JS_DupValue(ctx, prop));
                if (JS_IsException(v))
                    goto exception;
                v = js_json_check(ctx, jsc, val, v, prop);
                if (JS_IsException(v))
                    goto exception;
                if (!JS_IsUndefined(v)) {
                    if (has_content)
                        string_buffer_putc8(jsc->b, ',');
                    prop = JS_ToQuotedStringFree(ctx, prop);
                    if (JS_IsException(prop)) {
                        JS_FreeValue(ctx, v);
                        goto exception;
                    }
                    string_buffer_concat_value(jsc->b, sep);
                    string_buffer_concat_value(jsc->b, prop);
                    string_buffer_putc8(jsc->b, ':');
                    string_buffer_concat_value(jsc->b, sep1);
                    if (js_json_to_str(ctx, jsc, val, v, indent1))
                        goto exception;
                    has_content = TRUE;
                }
            }
            if (has_content && JS_VALUE_GET_STRING(jsc->gap)->len != 0) {
                string_buffer_putc8(jsc->b, '\n');
                string_buffer_concat_value(jsc->b, indent);
            }
            string_buffer_putc8(jsc->b, '}');
        }
        if (check_exception_free(ctx, js_array_pop(ctx, jsc->stack, 0, NULL, 0)))
            goto exception;
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, tab);
        JS_FreeValue(ctx, sep);
        JS_FreeValue(ctx, sep1);
        JS_FreeValue(ctx, indent1);
        JS_FreeValue(ctx, prop);
        return 0;
    case JS_TAG_STRING:
        val = JS_ToQuotedStringFree(ctx, val);
        if (JS_IsException(val))
            goto exception;
        goto concat_value;
    case JS_TAG_FLOAT64:
        if (!isfinite(JS_VALUE_GET_FLOAT64(val))) {
            val = JS_NULL;
        }
        goto concat_value;
    case JS_TAG_INT:
#ifdef CONFIG_BIGNUM
    case JS_TAG_BIG_FLOAT:
#endif
    case JS_TAG_BOOL:
    case JS_TAG_NULL:
    concat_value:
        return string_buffer_concat_value_free(jsc->b, val);
#ifdef CONFIG_BIGNUM
    case JS_TAG_BIG_INT:
        JS_ThrowTypeError(ctx, "bigint are forbidden in JSON.stringify");
        goto exception;
#endif
    default:
        JS_FreeValue(ctx, val);
        return 0;
    }
    
exception:
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, tab);
    JS_FreeValue(ctx, sep);
    JS_FreeValue(ctx, sep1);
    JS_FreeValue(ctx, indent1);
    JS_FreeValue(ctx, prop);
    return -1;
}

JSValue JS_JSONStringify(JSContext *ctx, JSValueConst obj,
                         JSValueConst replacer, JSValueConst space0)
{
    StringBuffer b_s;
    JSONStringifyContext jsc_s, *jsc = &jsc_s;
    JSValue val, v, space, ret, wrapper;
    int res;
    int64_t i, j, n;

    jsc->replacer_func = JS_UNDEFINED;
    jsc->stack = JS_UNDEFINED;
    jsc->property_list = JS_UNDEFINED;
    jsc->gap = JS_UNDEFINED;
    jsc->b = &b_s;
    jsc->empty = JS_AtomToString(ctx, JS_ATOM_empty_string);
    ret = JS_UNDEFINED;
    wrapper = JS_UNDEFINED;

    string_buffer_init(ctx, jsc->b, 0);
    jsc->stack = JS_NewArray(ctx);
    if (JS_IsException(jsc->stack))
        goto exception;
    if (JS_IsFunction(ctx, replacer)) {
        jsc->replacer_func = replacer;
    } else {
        res = JS_IsArray(ctx, replacer);
        if (res < 0)
            goto exception;
        if (res) {
            /* XXX: enumeration is not fully correct */
            jsc->property_list = JS_NewArray(ctx);
            if (JS_IsException(jsc->property_list))
                goto exception;
            if (js_get_length64(ctx, &n, replacer))
                goto exception;
            for (i = j = 0; i < n; i++) {
                JSValue present;
                v = JS_GetPropertyInt64(ctx, replacer, i);
                if (JS_IsException(v))
                    goto exception;
                if (JS_IsObject(v)) {
                    JSObject *p = JS_VALUE_GET_OBJ(v);
                    if (p->class_id == JS_CLASS_STRING ||
                        p->class_id == JS_CLASS_NUMBER) {
                        v = JS_ToStringFree(ctx, v);
                        if (JS_IsException(v))
                            goto exception;
                    } else {
                        JS_FreeValue(ctx, v);
                        continue;
                    }
                } else if (JS_IsNumber(v)) {
                    v = JS_ToStringFree(ctx, v);
                    if (JS_IsException(v))
                        goto exception;
                } else if (!JS_IsString(v)) {
                    JS_FreeValue(ctx, v);
                    continue;
                }
                present = js_array_includes(ctx, jsc->property_list,
                                            1, (JSValueConst *)&v);
                if (JS_IsException(present)) {
                    JS_FreeValue(ctx, v);
                    goto exception;
                }
                if (!JS_ToBoolFree(ctx, present)) {
                    JS_SetPropertyInt64(ctx, jsc->property_list, j++, v);
                } else {
                    JS_FreeValue(ctx, v);
                }
            }
        }
    }
    space = JS_DupValue(ctx, space0);
    if (JS_IsObject(space)) {
        JSObject *p = JS_VALUE_GET_OBJ(space);
        if (p->class_id == JS_CLASS_NUMBER) {
            space = JS_ToNumberFree(ctx, space);
        } else if (p->class_id == JS_CLASS_STRING) {
            space = JS_ToStringFree(ctx, space);
        }
        if (JS_IsException(space)) {
            JS_FreeValue(ctx, space);
            goto exception;
        }
    }
    if (JS_IsNumber(space)) {
        int n;
        if (JS_ToInt32Clamp(ctx, &n, space, 0, 10, 0))
            goto exception;
        jsc->gap = JS_NewStringLen(ctx, "          ", n);
    } else if (JS_IsString(space)) {
        JSString *p = JS_VALUE_GET_STRING(space);
        jsc->gap = js_sub_string(ctx, p, 0, min_int(p->len, 10));
    } else {
        jsc->gap = JS_DupValue(ctx, jsc->empty);
    }
    JS_FreeValue(ctx, space);
    if (JS_IsException(jsc->gap))
        goto exception;
    wrapper = JS_NewObject(ctx);
    if (JS_IsException(wrapper))
        goto exception;
    if (JS_DefinePropertyValue(ctx, wrapper, JS_ATOM_empty_string,
                               JS_DupValue(ctx, obj), JS_PROP_C_W_E) < 0)
        goto exception;
    val = JS_DupValue(ctx, obj);
                           
    val = js_json_check(ctx, jsc, wrapper, val, jsc->empty);
    if (JS_IsException(val))
        goto exception;
    if (JS_IsUndefined(val)) {
        ret = JS_UNDEFINED;
        goto done1;
    }
    if (js_json_to_str(ctx, jsc, wrapper, val, jsc->empty))
        goto exception;

    ret = string_buffer_end(jsc->b);
    goto done;

exception:
    ret = JS_EXCEPTION;
done1:
    string_buffer_free(jsc->b);
done:
    JS_FreeValue(ctx, wrapper);
    JS_FreeValue(ctx, jsc->empty);
    JS_FreeValue(ctx, jsc->gap);
    JS_FreeValue(ctx, jsc->property_list);
    JS_FreeValue(ctx, jsc->stack);
    return ret;
}

static JSValue js_json_stringify(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    // stringify(val, replacer, space)
    return JS_JSONStringify(ctx, argv[0], argv[1], argv[2]);
}

static const JSCFunctionListEntry js_json_funcs[] = {
    JS_CFUNC_DEF("parse", 2, js_json_parse ),
    JS_CFUNC_DEF("stringify", 3, js_json_stringify ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "JSON", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_json_obj[] = {
    JS_OBJECT_DEF("JSON", js_json_funcs, countof(js_json_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

void JS_AddIntrinsicJSON(JSContext *ctx)
{
    /* add JSON as autoinit object */
    JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_json_obj, countof(js_json_obj));
}

/* Reflect */

static JSValue js_reflect_apply(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return js_function_apply(ctx, argv[0], max_int(0, argc - 1), argv + 1, 2);
}

static JSValue js_reflect_construct(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValueConst func, array_arg, new_target;
    JSValue *tab, ret;
    uint32_t len;

    func = argv[0];
    array_arg = argv[1];
    if (argc > 2) {
        new_target = argv[2];
        if (!JS_IsConstructor(ctx, new_target))
            return JS_ThrowTypeError(ctx, "not a constructor");
    } else {
        new_target = func;
    }
    tab = build_arg_list(ctx, &len, array_arg);
    if (!tab)
        return JS_EXCEPTION;
    ret = JS_CallConstructor2(ctx, func, new_target, len, (JSValueConst *)tab);
    free_arg_list(ctx, tab, len);
    return ret;
}

static JSValue js_reflect_deleteProperty(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSValueConst obj;
    JSAtom atom;
    int ret;

    obj = argv[0];
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);
    atom = JS_ValueToAtom(ctx, argv[1]);
    if (unlikely(atom == JS_ATOM_NULL))
        return JS_EXCEPTION;
    ret = JS_DeleteProperty(ctx, obj, atom, 0);
    JS_FreeAtom(ctx, atom);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_reflect_get(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValueConst obj, prop, receiver;
    JSAtom atom;
    JSValue ret;

    obj = argv[0];
    prop = argv[1];
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (argc > 2)
        receiver = argv[2];
    else
        receiver = obj;
    atom = JS_ValueToAtom(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL))
        return JS_EXCEPTION;
    ret = JS_GetPropertyInternal(ctx, obj, atom, receiver, FALSE);
    JS_FreeAtom(ctx, atom);
    return ret;
}

static JSValue js_reflect_has(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValueConst obj, prop;
    JSAtom atom;
    int ret;

    obj = argv[0];
    prop = argv[1];
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);
    atom = JS_ValueToAtom(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL))
        return JS_EXCEPTION;
    ret = JS_HasProperty(ctx, obj, atom);
    JS_FreeAtom(ctx, atom);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_reflect_set(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValueConst obj, prop, val, receiver;
    int ret;
    JSAtom atom;

    obj = argv[0];
    prop = argv[1];
    val = argv[2];
    if (argc > 3)
        receiver = argv[3];
    else
        receiver = obj;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);
    atom = JS_ValueToAtom(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL))
        return JS_EXCEPTION;
    ret = JS_SetPropertyGeneric(ctx, obj, atom,
                                JS_DupValue(ctx, val), receiver, 0);
    JS_FreeAtom(ctx, atom);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_reflect_setPrototypeOf(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    int ret;
    ret = JS_SetPrototypeInternal(ctx, argv[0], argv[1], FALSE);
    if (ret < 0)
        return JS_EXCEPTION;
    else
        return JS_NewBool(ctx, ret);
}

static JSValue js_reflect_ownKeys(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    if (JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);
    return JS_GetOwnPropertyNames2(ctx, argv[0],
                                   JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK,
                                   JS_ITERATOR_KIND_KEY);
}

static const JSCFunctionListEntry js_reflect_funcs[] = {
    JS_CFUNC_DEF("apply", 3, js_reflect_apply ),
    JS_CFUNC_DEF("construct", 2, js_reflect_construct ),
    JS_CFUNC_MAGIC_DEF("defineProperty", 3, js_object_defineProperty, 1 ),
    JS_CFUNC_DEF("deleteProperty", 2, js_reflect_deleteProperty ),
    JS_CFUNC_DEF("get", 2, js_reflect_get ),
    JS_CFUNC_MAGIC_DEF("getOwnPropertyDescriptor", 2, js_object_getOwnPropertyDescriptor, 1 ),
    JS_CFUNC_MAGIC_DEF("getPrototypeOf", 1, js_object_getPrototypeOf, 1 ),
    JS_CFUNC_DEF("has", 2, js_reflect_has ),
    JS_CFUNC_MAGIC_DEF("isExtensible", 1, js_object_isExtensible, 1 ),
    JS_CFUNC_DEF("ownKeys", 1, js_reflect_ownKeys ),
    JS_CFUNC_MAGIC_DEF("preventExtensions", 1, js_object_preventExtensions, 1 ),
    JS_CFUNC_DEF("set", 3, js_reflect_set ),
    JS_CFUNC_DEF("setPrototypeOf", 2, js_reflect_setPrototypeOf ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Reflect", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_reflect_obj[] = {
    JS_OBJECT_DEF("Reflect", js_reflect_funcs, countof(js_reflect_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

/* Proxy */

static void js_proxy_finalizer(JSRuntime *rt, JSValue val)
{
    JSProxyData *s = JS_GetOpaque(val, JS_CLASS_PROXY);
    if (s) {
        JS_FreeValueRT(rt, s->target);
        JS_FreeValueRT(rt, s->handler);
        js_free_rt(rt, s);
    }
}

static void js_proxy_mark(JSRuntime *rt, JSValueConst val,
                          JS_MarkFunc *mark_func)
{
    JSProxyData *s = JS_GetOpaque(val, JS_CLASS_PROXY);
    if (s) {
        JS_MarkValue(rt, s->target, mark_func);
        JS_MarkValue(rt, s->handler, mark_func);
    }
}

static JSValue JS_ThrowTypeErrorRevokedProxy(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "revoked proxy");
}

static JSProxyData *get_proxy_method(JSContext *ctx, JSValue *pmethod,
                                     JSValueConst obj, JSAtom name)
{
    JSProxyData *s = JS_GetOpaque(obj, JS_CLASS_PROXY);
    JSValue method;

    /* safer to test recursion in all proxy methods */
    if (js_check_stack_overflow(ctx->rt, 0)) {
        JS_ThrowStackOverflow(ctx);
        return NULL;
    }
    
    /* 's' should never be NULL */
    if (s->is_revoked) {
        JS_ThrowTypeErrorRevokedProxy(ctx);
        return NULL;
    }
    method = JS_GetProperty(ctx, s->handler, name);
    if (JS_IsException(method))
        return NULL;
    if (JS_IsNull(method))
        method = JS_UNDEFINED;
    *pmethod = method;
    return s;
}

static JSValue js_proxy_getPrototypeOf(JSContext *ctx, JSValueConst obj)
{
    JSProxyData *s;
    JSValue method, ret, proto1;
    int res;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_getPrototypeOf);
    if (!s)
        return JS_EXCEPTION;
    if (JS_IsUndefined(method))
        return JS_GetPrototype(ctx, s->target);
    ret = JS_CallFree(ctx, method, s->handler, 1, (JSValueConst *)&s->target);
    if (JS_IsException(ret))
        return ret;
    if (JS_VALUE_GET_TAG(ret) != JS_TAG_NULL &&
        JS_VALUE_GET_TAG(ret) != JS_TAG_OBJECT) {
        goto fail;
    }
    res = JS_IsExtensible(ctx, s->target);
    if (res < 0) {
        JS_FreeValue(ctx, ret);
        return JS_EXCEPTION;
    }
    if (!res) {
        /* check invariant */
        proto1 = JS_GetPrototype(ctx, s->target);
        if (JS_IsException(proto1)) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        if (JS_VALUE_GET_OBJ(proto1) != JS_VALUE_GET_OBJ(ret)) {
            JS_FreeValue(ctx, proto1);
        fail:
            JS_FreeValue(ctx, ret);
            return JS_ThrowTypeError(ctx, "proxy: inconsistent prototype");
        }
        JS_FreeValue(ctx, proto1);
    }
    return ret;
}

static int js_proxy_setPrototypeOf(JSContext *ctx, JSValueConst obj,
                                   JSValueConst proto_val, BOOL throw_flag)
{
    JSProxyData *s;
    JSValue method, ret, proto1;
    JSValueConst args[2];
    BOOL res;
    int res2;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_setPrototypeOf);
    if (!s)
        return -1;
    if (JS_IsUndefined(method))
        return JS_SetPrototypeInternal(ctx, s->target, proto_val, throw_flag);
    args[0] = s->target;
    args[1] = proto_val;
    ret = JS_CallFree(ctx, method, s->handler, 2, args);
    if (JS_IsException(ret))
        return -1;
    res = JS_ToBoolFree(ctx, ret);
    if (!res) {
        if (throw_flag) {
            JS_ThrowTypeError(ctx, "proxy: bad prototype");
            return -1;
        } else {
            return FALSE;
        }
    }
    res2 = JS_IsExtensible(ctx, s->target);
    if (res2 < 0)
        return -1;
    if (!res2) {
        proto1 = JS_GetPrototype(ctx, s->target);
        if (JS_IsException(proto1))
            return -1;
        if (JS_VALUE_GET_OBJ(proto_val) != JS_VALUE_GET_OBJ(proto1)) {
            JS_FreeValue(ctx, proto1);
            JS_ThrowTypeError(ctx, "proxy: inconsistent prototype");
            return -1;
        }
        JS_FreeValue(ctx, proto1);
    }
    return TRUE;
}

static int js_proxy_isExtensible(JSContext *ctx, JSValueConst obj)
{
    JSProxyData *s;
    JSValue method, ret;
    BOOL res;
    int res2;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_isExtensible);
    if (!s)
        return -1;
    if (JS_IsUndefined(method))
        return JS_IsExtensible(ctx, s->target);
    ret = JS_CallFree(ctx, method, s->handler, 1, (JSValueConst *)&s->target);
    if (JS_IsException(ret))
        return -1;
    res = JS_ToBoolFree(ctx, ret);
    res2 = JS_IsExtensible(ctx, s->target);
    if (res2 < 0)
        return res2;
    if (res != res2) {
        JS_ThrowTypeError(ctx, "proxy: inconsistent isExtensible");
        return -1;
    }
    return res;
}

static int js_proxy_preventExtensions(JSContext *ctx, JSValueConst obj)
{
    JSProxyData *s;
    JSValue method, ret;
    BOOL res;
    int res2;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_preventExtensions);
    if (!s)
        return -1;
    if (JS_IsUndefined(method))
        return JS_PreventExtensions(ctx, s->target);
    ret = JS_CallFree(ctx, method, s->handler, 1, (JSValueConst *)&s->target);
    if (JS_IsException(ret))
        return -1;
    res = JS_ToBoolFree(ctx, ret);
    if (res) {
        res2 = JS_IsExtensible(ctx, s->target);
        if (res2 < 0)
            return res2;
        if (res2) {
            JS_ThrowTypeError(ctx, "proxy: inconsistent preventExtensions");
            return -1;
        }
    }
    return res;
}

static int js_proxy_has(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
    JSProxyData *s;
    JSValue method, ret1, atom_val;
    int ret, res;
    JSObject *p;
    JSValueConst args[2];
    BOOL res2;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_has);
    if (!s)
        return -1;
    if (JS_IsUndefined(method))
        return JS_HasProperty(ctx, s->target, atom);
    atom_val = JS_AtomToValue(ctx, atom);
    if (JS_IsException(atom_val)) {
        JS_FreeValue(ctx, method);
        return -1;
    }
    args[0] = s->target;
    args[1] = atom_val;
    ret1 = JS_CallFree(ctx, method, s->handler, 2, args);
    JS_FreeValue(ctx, atom_val);
    if (JS_IsException(ret1))
        return -1;
    ret = JS_ToBoolFree(ctx, ret1);
    if (!ret) {
        JSPropertyDescriptor desc;
        p = JS_VALUE_GET_OBJ(s->target);
        res = JS_GetOwnPropertyInternal(ctx, &desc, p, atom);
        if (res < 0)
            return -1;
        if (res) {
            res2 = !(desc.flags & JS_PROP_CONFIGURABLE);
            js_free_desc(ctx, &desc);
            if (res2 || !p->extensible) {
                JS_ThrowTypeError(ctx, "proxy: inconsistent has");
                return -1;
            }
        }
    }
    return ret;
}

static JSValue js_proxy_get(JSContext *ctx, JSValueConst obj, JSAtom atom,
                            JSValueConst receiver)
{
    JSProxyData *s;
    JSValue method, ret, atom_val;
    int res;
    JSValueConst args[3];
    JSPropertyDescriptor desc;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_get);
    if (!s)
        return JS_EXCEPTION;
    /* Note: recursion is possible thru the prototype of s->target */
    if (JS_IsUndefined(method))
        return JS_GetPropertyInternal(ctx, s->target, atom, receiver, FALSE);
    atom_val = JS_AtomToValue(ctx, atom);
    if (JS_IsException(atom_val)) {
        JS_FreeValue(ctx, method);
        return JS_EXCEPTION;
    }
    args[0] = s->target;
    args[1] = atom_val;
    args[2] = receiver;
    ret = JS_CallFree(ctx, method, s->handler, 3, args);
    JS_FreeValue(ctx, atom_val);
    if (JS_IsException(ret))
        return JS_EXCEPTION;
    res = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(s->target), atom);
    if (res < 0)
        return JS_EXCEPTION;
    if (res) {
        if ((desc.flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0) {
            if (!js_same_value(ctx, desc.value, ret)) {
                goto fail;
            }
        } else if ((desc.flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE)) == JS_PROP_GETSET) {
            if (JS_IsUndefined(desc.getter) && !JS_IsUndefined(ret)) {
            fail:
                js_free_desc(ctx, &desc);
                JS_FreeValue(ctx, ret);
                return JS_ThrowTypeError(ctx, "proxy: inconsistent get");
            }
        }
        js_free_desc(ctx, &desc);
    }
    return ret;
}

static int js_proxy_set(JSContext *ctx, JSValueConst obj, JSAtom atom,
                        JSValueConst value, JSValueConst receiver, int flags)
{
    JSProxyData *s;
    JSValue method, ret1, atom_val;
    int ret, res;
    JSValueConst args[4];

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_set);
    if (!s)
        return -1;
    if (JS_IsUndefined(method)) {
        return JS_SetPropertyGeneric(ctx, s->target, atom,
                                     JS_DupValue(ctx, value), receiver,
                                     flags);
    }
    atom_val = JS_AtomToValue(ctx, atom);
    if (JS_IsException(atom_val)) {
        JS_FreeValue(ctx, method);
        return -1;
    }
    args[0] = s->target;
    args[1] = atom_val;
    args[2] = value;
    args[3] = receiver;
    ret1 = JS_CallFree(ctx, method, s->handler, 4, args);
    JS_FreeValue(ctx, atom_val);
    if (JS_IsException(ret1))
        return -1;
    ret = JS_ToBoolFree(ctx, ret1);
    if (ret) {
        JSPropertyDescriptor desc;
        res = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(s->target), atom);
        if (res < 0)
            return -1;
        if (res) {
            if ((desc.flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0) {
                if (!js_same_value(ctx, desc.value, value)) {
                    goto fail;
                }
            } else if ((desc.flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE)) == JS_PROP_GETSET && JS_IsUndefined(desc.setter)) {
                fail:
                    js_free_desc(ctx, &desc);
                    JS_ThrowTypeError(ctx, "proxy: inconsistent set");
                    return -1;
            }
            js_free_desc(ctx, &desc);
        }
    } else {
        if ((flags & JS_PROP_THROW) ||
            ((flags & JS_PROP_THROW_STRICT) && is_strict_mode(ctx))) {
            JS_ThrowTypeError(ctx, "proxy: cannot set property");
            return -1;
        }
    }
    return ret;
}

static JSValue js_create_desc(JSContext *ctx, JSValueConst val,
                              JSValueConst getter, JSValueConst setter,
                              int flags)
{
    JSValue ret;
    ret = JS_NewObject(ctx);
    if (JS_IsException(ret))
        return ret;
    if (flags & JS_PROP_HAS_GET) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_get, JS_DupValue(ctx, getter),
                               JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_SET) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_set, JS_DupValue(ctx, setter),
                               JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_VALUE) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_value, JS_DupValue(ctx, val),
                               JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_WRITABLE) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_writable,
                               JS_NewBool(ctx, (flags & JS_PROP_WRITABLE) != 0),
                               JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_ENUMERABLE) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_enumerable,
                               JS_NewBool(ctx, (flags & JS_PROP_ENUMERABLE) != 0),
                               JS_PROP_C_W_E);
    }
    if (flags & JS_PROP_HAS_CONFIGURABLE) {
        JS_DefinePropertyValue(ctx, ret, JS_ATOM_configurable,
                               JS_NewBool(ctx, (flags & JS_PROP_CONFIGURABLE) != 0),
                               JS_PROP_C_W_E);
    }
    return ret;
}

static int js_proxy_get_own_property(JSContext *ctx, JSPropertyDescriptor *pdesc,
                                     JSValueConst obj, JSAtom prop)
{
    JSProxyData *s;
    JSValue method, trap_result_obj, prop_val;
    int res, target_desc_ret, ret;
    JSObject *p;
    JSValueConst args[2];
    JSPropertyDescriptor result_desc, target_desc;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_getOwnPropertyDescriptor);
    if (!s)
        return -1;
    p = JS_VALUE_GET_OBJ(s->target);
    if (JS_IsUndefined(method)) {
        return JS_GetOwnPropertyInternal(ctx, pdesc, p, prop);
    }
    prop_val = JS_AtomToValue(ctx, prop);
    if (JS_IsException(prop_val)) {
        JS_FreeValue(ctx, method);
        return -1;
    }
    args[0] = s->target;
    args[1] = prop_val;
    trap_result_obj = JS_CallFree(ctx, method, s->handler, 2, args);
    JS_FreeValue(ctx, prop_val);
    if (JS_IsException(trap_result_obj))
        return -1;
    if (!JS_IsObject(trap_result_obj) && !JS_IsUndefined(trap_result_obj)) {
        JS_FreeValue(ctx, trap_result_obj);
        goto fail;
    }
    target_desc_ret = JS_GetOwnPropertyInternal(ctx, &target_desc, p, prop);
    if (target_desc_ret < 0) {
        JS_FreeValue(ctx, trap_result_obj);
        return -1;
    }
    if (target_desc_ret)
        js_free_desc(ctx, &target_desc);
    if (JS_IsUndefined(trap_result_obj)) {
        if (target_desc_ret) {
            if (!(target_desc.flags & JS_PROP_CONFIGURABLE) || !p->extensible)
                goto fail;
        }
        ret = FALSE;
    } else {
        int flags1, extensible_target;
        extensible_target = JS_IsExtensible(ctx, s->target);
        if (extensible_target < 0) {
            JS_FreeValue(ctx, trap_result_obj);
            return -1;
        }
        res = js_obj_to_desc(ctx, &result_desc, trap_result_obj);
        JS_FreeValue(ctx, trap_result_obj);
        if (res < 0)
            return -1;
        
        if (target_desc_ret) {
            /* convert result_desc.flags to defineProperty flags */
            flags1 = result_desc.flags | JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE;
            if (result_desc.flags & JS_PROP_GETSET)
                flags1 |= JS_PROP_HAS_GET | JS_PROP_HAS_SET;
            else
                flags1 |= JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE;
            /* XXX: not complete check: need to compare value &
               getter/setter as in defineproperty */
            if (!check_define_prop_flags(target_desc.flags, flags1))
                goto fail1;
        } else {
            if (!extensible_target)
                goto fail1;
        }
        if (!(result_desc.flags & JS_PROP_CONFIGURABLE)) {
            if (!target_desc_ret || (target_desc.flags & JS_PROP_CONFIGURABLE))
                goto fail1;
            if ((result_desc.flags &
                 (JS_PROP_GETSET | JS_PROP_WRITABLE)) == 0 &&
                target_desc_ret &&
                (target_desc.flags & JS_PROP_WRITABLE) != 0) {
                /* proxy-missing-checks */
            fail1:
                js_free_desc(ctx, &result_desc);
            fail:
                JS_ThrowTypeError(ctx, "proxy: inconsistent getOwnPropertyDescriptor");
                return -1;
            }
        }
        ret = TRUE;
        if (pdesc) {
            *pdesc = result_desc;
        } else {
            js_free_desc(ctx, &result_desc);
        }
    }
    return ret;
}

static int js_proxy_define_own_property(JSContext *ctx, JSValueConst obj,
                                        JSAtom prop, JSValueConst val,
                                        JSValueConst getter, JSValueConst setter,
                                        int flags)
{
    JSProxyData *s;
    JSValue method, ret1, prop_val, desc_val;
    int res, ret;
    JSObject *p;
    JSValueConst args[3];
    JSPropertyDescriptor desc;
    BOOL setting_not_configurable;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_defineProperty);
    if (!s)
        return -1;
    if (JS_IsUndefined(method)) {
        return JS_DefineProperty(ctx, s->target, prop, val, getter, setter, flags);
    }
    prop_val = JS_AtomToValue(ctx, prop);
    if (JS_IsException(prop_val)) {
        JS_FreeValue(ctx, method);
        return -1;
    }
    desc_val = js_create_desc(ctx, val, getter, setter, flags);
    if (JS_IsException(desc_val)) {
        JS_FreeValue(ctx, prop_val);
        JS_FreeValue(ctx, method);
        return -1;
    }
    args[0] = s->target;
    args[1] = prop_val;
    args[2] = desc_val;
    ret1 = JS_CallFree(ctx, method, s->handler, 3, args);
    JS_FreeValue(ctx, prop_val);
    JS_FreeValue(ctx, desc_val);
    if (JS_IsException(ret1))
        return -1;
    ret = JS_ToBoolFree(ctx, ret1);
    if (!ret) {
        if (flags & JS_PROP_THROW) {
            JS_ThrowTypeError(ctx, "proxy: defineProperty exception");
            return -1;
        } else {
            return 0;
        }
    }
    p = JS_VALUE_GET_OBJ(s->target);
    res = JS_GetOwnPropertyInternal(ctx, &desc, p, prop);
    if (res < 0)
        return -1;
    setting_not_configurable = ((flags & (JS_PROP_HAS_CONFIGURABLE |
                                          JS_PROP_CONFIGURABLE)) ==
                                JS_PROP_HAS_CONFIGURABLE);
    if (!res) {
        if (!p->extensible || setting_not_configurable)
            goto fail;
    } else {
        if (!check_define_prop_flags(desc.flags, flags) ||
            ((desc.flags & JS_PROP_CONFIGURABLE) && setting_not_configurable)) {
            goto fail1;
        }
        if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
            if ((desc.flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE)) ==
                JS_PROP_GETSET) {
                if ((flags & JS_PROP_HAS_GET) &&
                    !js_same_value(ctx, getter, desc.getter)) {
                    goto fail1;
                }
                if ((flags & JS_PROP_HAS_SET) &&
                    !js_same_value(ctx, setter, desc.setter)) {
                    goto fail1;
                }
            }
        } else if (flags & JS_PROP_HAS_VALUE) {
            if ((desc.flags & (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) ==
                JS_PROP_WRITABLE && !(flags & JS_PROP_WRITABLE)) {
                /* missing-proxy-check feature */
                goto fail1;
            } else if ((desc.flags & (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0 &&
                !js_same_value(ctx, val, desc.value)) {
                goto fail1;
            }
        }
        if (flags & JS_PROP_HAS_WRITABLE) {
            if ((desc.flags & (JS_PROP_GETSET | JS_PROP_CONFIGURABLE |
                               JS_PROP_WRITABLE)) == JS_PROP_WRITABLE) {
                /* proxy-missing-checks */
            fail1:
                js_free_desc(ctx, &desc);
            fail:
                JS_ThrowTypeError(ctx, "proxy: inconsistent defineProperty");
                return -1;
            }
        }
        js_free_desc(ctx, &desc);
    }
    return 1;
}

static int js_proxy_delete_property(JSContext *ctx, JSValueConst obj,
                                    JSAtom atom)
{
    JSProxyData *s;
    JSValue method, ret, atom_val;
    int res, res2, is_extensible;
    JSValueConst args[2];

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_deleteProperty);
    if (!s)
        return -1;
    if (JS_IsUndefined(method)) {
        return JS_DeleteProperty(ctx, s->target, atom, 0);
    }
    atom_val = JS_AtomToValue(ctx, atom);;
    if (JS_IsException(atom_val)) {
        JS_FreeValue(ctx, method);
        return -1;
    }
    args[0] = s->target;
    args[1] = atom_val;
    ret = JS_CallFree(ctx, method, s->handler, 2, args);
    JS_FreeValue(ctx, atom_val);
    if (JS_IsException(ret))
        return -1;
    res = JS_ToBoolFree(ctx, ret);
    if (res) {
        JSPropertyDescriptor desc;
        res2 = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(s->target), atom);
        if (res2 < 0)
            return -1;
        if (res2) {
            if (!(desc.flags & JS_PROP_CONFIGURABLE))
                goto fail;
            is_extensible = JS_IsExtensible(ctx, s->target);
            if (is_extensible < 0)
                goto fail1;
            if (!is_extensible) {
                /* proxy-missing-checks */
            fail:
                JS_ThrowTypeError(ctx, "proxy: inconsistent deleteProperty");
            fail1:
                js_free_desc(ctx, &desc);
                return -1;
            }
            js_free_desc(ctx, &desc);
        }
    }
    return res;
}

/* return the index of the property or -1 if not found */
static int find_prop_key(const JSPropertyEnum *tab, int n, JSAtom atom)
{
    int i;
    for(i = 0; i < n; i++) {
        if (tab[i].atom == atom)
            return i;
    }
    return -1;
}

static int js_proxy_get_own_property_names(JSContext *ctx,
                                           JSPropertyEnum **ptab,
                                           uint32_t *plen,
                                           JSValueConst obj)
{
    JSProxyData *s;
    JSValue method, prop_array, val;
    uint32_t len, i, len2;
    JSPropertyEnum *tab, *tab2;
    JSAtom atom;
    JSPropertyDescriptor desc;
    int res, is_extensible, idx;

    s = get_proxy_method(ctx, &method, obj, JS_ATOM_ownKeys);
    if (!s)
        return -1;
    if (JS_IsUndefined(method)) {
        return JS_GetOwnPropertyNamesInternal(ctx, ptab, plen,
                                      JS_VALUE_GET_OBJ(s->target),
                                      JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK);
    }
    prop_array = JS_CallFree(ctx, method, s->handler, 1, (JSValueConst *)&s->target);
    if (JS_IsException(prop_array))
        return -1;
    tab = NULL;
    len = 0;
    tab2 = NULL;
    len2 = 0;
    if (js_get_length32(ctx, &len, prop_array))
        goto fail;
    if (len > 0) {
        tab = js_mallocz(ctx, sizeof(tab[0]) * len);
        if (!tab)
            goto fail;
    }
    for(i = 0; i < len; i++) {
        val = JS_GetPropertyUint32(ctx, prop_array, i);
        if (JS_IsException(val))
            goto fail;
        if (!JS_IsString(val) && !JS_IsSymbol(val)) {
            JS_FreeValue(ctx, val);
            JS_ThrowTypeError(ctx, "proxy: properties must be strings or symbols");
            goto fail;
        }
        atom = JS_ValueToAtom(ctx, val);
        JS_FreeValue(ctx, val);
        if (atom == JS_ATOM_NULL)
            goto fail;
        tab[i].atom = atom;
        tab[i].is_enumerable = FALSE; /* XXX: redundant? */
    }

    /* check duplicate properties (XXX: inefficient, could store the
     * properties an a temporary object to use the hash) */
    for(i = 1; i < len; i++) {
        if (find_prop_key(tab, i, tab[i].atom) >= 0) {
            JS_ThrowTypeError(ctx, "proxy: duplicate property");
            goto fail;
        }
    }

    is_extensible = JS_IsExtensible(ctx, s->target);
    if (is_extensible < 0)
        goto fail;

    /* check if there are non configurable properties */
    if (s->is_revoked) {
        JS_ThrowTypeErrorRevokedProxy(ctx);
        goto fail;
    }
    if (JS_GetOwnPropertyNamesInternal(ctx, &tab2, &len2, JS_VALUE_GET_OBJ(s->target),
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK))
        goto fail;
    for(i = 0; i < len2; i++) {
        if (s->is_revoked) {
            JS_ThrowTypeErrorRevokedProxy(ctx);
            goto fail;
        }
        res = JS_GetOwnPropertyInternal(ctx, &desc, JS_VALUE_GET_OBJ(s->target),
                                tab2[i].atom);
        if (res < 0)
            goto fail;
        if (res) {  /* safety, property should be found */
            js_free_desc(ctx, &desc);
            if (!(desc.flags & JS_PROP_CONFIGURABLE) || !is_extensible) {
                idx = find_prop_key(tab, len, tab2[i].atom);
                if (idx < 0) {
                    JS_ThrowTypeError(ctx, "proxy: target property must be present in proxy ownKeys");
                    goto fail;
                }
                /* mark the property as found */
                if (!is_extensible)
                    tab[idx].is_enumerable = TRUE;
            }
        }
    }
    if (!is_extensible) {
        /* check that all property in 'tab' were checked */
        for(i = 0; i < len; i++) {
            if (!tab[i].is_enumerable) {
                JS_ThrowTypeError(ctx, "proxy: property not present in target were returned by non extensible proxy");
                goto fail;
            }
        }
    }

    js_free_prop_enum(ctx, tab2, len2);
    JS_FreeValue(ctx, prop_array);
    *ptab = tab;
    *plen = len;
    return 0;
 fail:
    js_free_prop_enum(ctx, tab2, len2);
    js_free_prop_enum(ctx, tab, len);
    JS_FreeValue(ctx, prop_array);
    return -1;
}

static JSValue js_proxy_call_constructor(JSContext *ctx, JSValueConst func_obj,
                                         JSValueConst new_target,
                                         int argc, JSValueConst *argv)
{
    JSProxyData *s;
    JSValue method, arg_array, ret;
    JSValueConst args[3];

    s = get_proxy_method(ctx, &method, func_obj, JS_ATOM_construct);
    if (!s)
        return JS_EXCEPTION;
    if (!JS_IsConstructor(ctx, s->target))
        return JS_ThrowTypeError(ctx, "not a constructor");
    if (JS_IsUndefined(method))
        return JS_CallConstructor2(ctx, s->target, new_target, argc, argv);
    arg_array = js_create_array(ctx, argc, argv);
    if (JS_IsException(arg_array)) {
        ret = JS_EXCEPTION;
        goto fail;
    }
    args[0] = s->target;
    args[1] = arg_array;
    args[2] = new_target;
    ret = JS_Call(ctx, method, s->handler, 3, args);
    if (!JS_IsException(ret) && JS_VALUE_GET_TAG(ret) != JS_TAG_OBJECT) {
        JS_FreeValue(ctx, ret);
        ret = JS_ThrowTypeErrorNotAnObject(ctx);
    }
 fail:
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, arg_array);
    return ret;
}

static JSValue js_proxy_call(JSContext *ctx, JSValueConst func_obj,
                             JSValueConst this_obj,
                             int argc, JSValueConst *argv, int flags)
{
    JSProxyData *s;
    JSValue method, arg_array, ret;
    JSValueConst args[3];

    if (flags & JS_CALL_FLAG_CONSTRUCTOR)
        return js_proxy_call_constructor(ctx, func_obj, this_obj, argc, argv);
    
    s = get_proxy_method(ctx, &method, func_obj, JS_ATOM_apply);
    if (!s)
        return JS_EXCEPTION;
    if (!s->is_func) {
        JS_FreeValue(ctx, method);
        return JS_ThrowTypeError(ctx, "not a function");
    }
    if (JS_IsUndefined(method))
        return JS_Call(ctx, s->target, this_obj, argc, argv);
    arg_array = js_create_array(ctx, argc, argv);
    if (JS_IsException(arg_array)) {
        ret = JS_EXCEPTION;
        goto fail;
    }
    args[0] = s->target;
    args[1] = this_obj;
    args[2] = arg_array;
    ret = JS_Call(ctx, method, s->handler, 3, args);
 fail:
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, arg_array);
    return ret;
}

static int js_proxy_isArray(JSContext *ctx, JSValueConst obj)
{
    JSProxyData *s = JS_GetOpaque(obj, JS_CLASS_PROXY);
    if (!s)
        return FALSE;
    if (s->is_revoked) {
        JS_ThrowTypeErrorRevokedProxy(ctx);
        return -1;
    }
    return JS_IsArray(ctx, s->target);
}

static const JSClassExoticMethods js_proxy_exotic_methods = {
    .get_own_property = js_proxy_get_own_property,
    .define_own_property = js_proxy_define_own_property,
    .delete_property = js_proxy_delete_property,
    .get_own_property_names = js_proxy_get_own_property_names,
    .has_property = js_proxy_has,
    .get_property = js_proxy_get,
    .set_property = js_proxy_set,
};

static JSValue js_proxy_constructor(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValueConst target, handler;
    JSValue obj;
    JSProxyData *s;

    target = argv[0];
    handler = argv[1];
    if (JS_VALUE_GET_TAG(target) != JS_TAG_OBJECT ||
        JS_VALUE_GET_TAG(handler) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);

    obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_PROXY);
    if (JS_IsException(obj))
        return obj;
    s = js_malloc(ctx, sizeof(JSProxyData));
    if (!s) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    s->target = JS_DupValue(ctx, target);
    s->handler = JS_DupValue(ctx, handler);
    s->is_func = JS_IsFunction(ctx, target);
    s->is_revoked = FALSE;
    JS_SetOpaque(obj, s);
    JS_SetConstructorBit(ctx, obj, JS_IsConstructor(ctx, target));
    return obj;
}

static JSValue js_proxy_revoke(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic,
                               JSValue *func_data)
{
    JSProxyData *s = JS_GetOpaque(func_data[0], JS_CLASS_PROXY);
    if (s) {
        /* We do not free the handler and target in case they are
           referenced as constants in the C call stack */
        s->is_revoked = TRUE;
        JS_FreeValue(ctx, func_data[0]);
        func_data[0] = JS_NULL;
    }
    return JS_UNDEFINED;
}

static JSValue js_proxy_revoke_constructor(JSContext *ctx,
                                           JSValueConst proxy_obj)
{
    return JS_NewCFunctionData(ctx, js_proxy_revoke, 0, 0, 1, &proxy_obj);
}

static JSValue js_proxy_revocable(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue proxy_obj, revoke_obj = JS_UNDEFINED, obj;

    proxy_obj = js_proxy_constructor(ctx, JS_UNDEFINED, argc, argv);
    if (JS_IsException(proxy_obj))
        goto fail;
    revoke_obj = js_proxy_revoke_constructor(ctx, proxy_obj);
    if (JS_IsException(revoke_obj))
        goto fail;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        goto fail;
    // XXX: exceptions?
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_proxy, proxy_obj, JS_PROP_C_W_E);
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_revoke, revoke_obj, JS_PROP_C_W_E);
    return obj;
 fail:
    JS_FreeValue(ctx, proxy_obj);
    JS_FreeValue(ctx, revoke_obj);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_proxy_funcs[] = {
    JS_CFUNC_DEF("revocable", 2, js_proxy_revocable ),
};

static const JSClassShortDef js_proxy_class_def[] = {
    { JS_ATOM_Object, js_proxy_finalizer, js_proxy_mark }, /* JS_CLASS_PROXY */
};

void JS_AddIntrinsicProxy(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValue obj1;

    if (!JS_IsRegisteredClass(rt, JS_CLASS_PROXY)) {
        init_class_range(rt, js_proxy_class_def, JS_CLASS_PROXY,
                         countof(js_proxy_class_def));
        rt->class_array[JS_CLASS_PROXY].exotic = &js_proxy_exotic_methods;
        rt->class_array[JS_CLASS_PROXY].call = js_proxy_call;
    }

    obj1 = JS_NewCFunction2(ctx, js_proxy_constructor, "Proxy", 2,
                            JS_CFUNC_constructor, 0);
    JS_SetConstructorBit(ctx, obj1, TRUE);
    JS_SetPropertyFunctionList(ctx, obj1, js_proxy_funcs,
                               countof(js_proxy_funcs));
    JS_DefinePropertyValueStr(ctx, ctx->global_obj, "Proxy",
                              obj1, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

/* Symbol */

static JSValue js_symbol_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue str;
    JSString *p;

    if (!JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "not a constructor");
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        p = NULL;
    } else {
        str = JS_ToString(ctx, argv[0]);
        if (JS_IsException(str))
            return JS_EXCEPTION;
        p = JS_VALUE_GET_STRING(str);
    }
    return JS_NewSymbol(ctx, p, JS_ATOM_TYPE_SYMBOL);
}

static JSValue js_thisSymbolValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_SYMBOL)
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_SYMBOL) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_SYMBOL)
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a symbol");
}

static JSValue js_symbol_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue val, ret;
    val = js_thisSymbolValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    /* XXX: use JS_ToStringInternal() with a flags */
    ret = js_string_constructor(ctx, JS_UNDEFINED, 1, (JSValueConst *)&val);
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_symbol_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_thisSymbolValue(ctx, this_val);
}

static JSValue js_symbol_get_description(JSContext *ctx, JSValueConst this_val)
{
    JSValue val, ret;
    JSAtomStruct *p;

    val = js_thisSymbolValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    p = JS_VALUE_GET_PTR(val);
    if (p->len == 0 && p->is_wide_char != 0) {
        ret = JS_UNDEFINED;
    } else {
        ret = JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p));
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static const JSCFunctionListEntry js_symbol_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_symbol_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_symbol_valueOf ),
    // XXX: should have writable: false
    JS_CFUNC_DEF("[Symbol.toPrimitive]", 1, js_symbol_valueOf ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Symbol", JS_PROP_CONFIGURABLE ),
    JS_CGETSET_DEF("description", js_symbol_get_description, NULL ),
};

static JSValue js_symbol_for(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSValue str;

    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return JS_EXCEPTION;
    return JS_NewSymbol(ctx, JS_VALUE_GET_STRING(str), JS_ATOM_TYPE_GLOBAL_SYMBOL);
}

static JSValue js_symbol_keyFor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSAtomStruct *p;

    if (!JS_IsSymbol(argv[0]))
        return JS_ThrowTypeError(ctx, "not a symbol");
    p = JS_VALUE_GET_PTR(argv[0]);
    if (p->atom_type != JS_ATOM_TYPE_GLOBAL_SYMBOL)
        return JS_UNDEFINED;
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_STRING, p));
}

static const JSCFunctionListEntry js_symbol_funcs[] = {
    JS_CFUNC_DEF("for", 1, js_symbol_for ),
    JS_CFUNC_DEF("keyFor", 1, js_symbol_keyFor ),
};

/* Set/Map/WeakSet/WeakMap */

typedef struct JSMapRecord {
    int ref_count; /* used during enumeration to avoid freeing the record */
    BOOL empty; /* TRUE if the record is deleted */
    struct JSMapState *map;
    struct JSMapRecord *next_weak_ref;
    struct list_head link;
    struct list_head hash_link;
    JSValue key;
    JSValue value;
} JSMapRecord;

typedef struct JSMapState {
    BOOL is_weak; /* TRUE if WeakSet/WeakMap */
    struct list_head records; /* list of JSMapRecord.link */
    uint32_t record_count;
    struct list_head *hash_table;
    uint32_t hash_size; /* must be a power of two */
    uint32_t record_count_threshold; /* count at which a hash table
                                        resize is needed */
} JSMapState;

#define MAGIC_SET (1 << 0)
#define MAGIC_WEAK (1 << 1)

static JSValue js_map_constructor(JSContext *ctx, JSValueConst new_target,
                                  int argc, JSValueConst *argv, int magic)
{
    JSMapState *s;
    JSValue obj, adder = JS_UNDEFINED, iter = JS_UNDEFINED, next_method = JS_UNDEFINED;
    JSValueConst arr;
    BOOL is_set, is_weak;

    is_set = magic & MAGIC_SET;
    is_weak = ((magic & MAGIC_WEAK) != 0);
    obj = js_create_from_ctor(ctx, new_target, JS_CLASS_MAP + magic);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        goto fail;
    init_list_head(&s->records);
    s->is_weak = is_weak;
    JS_SetOpaque(obj, s);
    s->hash_size = 1;
    s->hash_table = js_malloc(ctx, sizeof(s->hash_table[0]) * s->hash_size);
    if (!s->hash_table)
        goto fail;
    init_list_head(&s->hash_table[0]);
    s->record_count_threshold = 4;

    arr = JS_UNDEFINED;
    if (argc > 0)
        arr = argv[0];
    if (!JS_IsUndefined(arr) && !JS_IsNull(arr)) {
        JSValue item, ret;
        BOOL done;

        adder = JS_GetProperty(ctx, obj, is_set ? JS_ATOM_add : JS_ATOM_set);
        if (JS_IsException(adder))
            goto fail;
        if (!JS_IsFunction(ctx, adder)) {
            JS_ThrowTypeError(ctx, "set/add is not a function");
            goto fail;
        }

        iter = JS_GetIterator(ctx, arr, FALSE);
        if (JS_IsException(iter))
            goto fail;
        next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next_method))
            goto fail;

        for(;;) {
            item = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
            if (JS_IsException(item))
                goto fail;
            if (done) {
                JS_FreeValue(ctx, item);
                break;
            }
            if (is_set) {
                ret = JS_Call(ctx, adder, obj, 1, (JSValueConst *)&item);
                if (JS_IsException(ret)) {
                    JS_FreeValue(ctx, item);
                    goto fail;
                }
            } else {
                JSValue key, value;
                JSValueConst args[2];
                key = JS_UNDEFINED;
                value = JS_UNDEFINED;
                if (!JS_IsObject(item)) {
                    JS_ThrowTypeErrorNotAnObject(ctx);
                    goto fail1;
                }
                key = JS_GetPropertyUint32(ctx, item, 0);
                if (JS_IsException(key))
                    goto fail1;
                value = JS_GetPropertyUint32(ctx, item, 1);
                if (JS_IsException(value))
                    goto fail1;
                args[0] = key;
                args[1] = value;
                ret = JS_Call(ctx, adder, obj, 2, args);
                if (JS_IsException(ret)) {
                fail1:
                    JS_FreeValue(ctx, item);
                    JS_FreeValue(ctx, key);
                    JS_FreeValue(ctx, value);
                    goto fail;
                }
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, value);
            }
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, item);
        }
        JS_FreeValue(ctx, next_method);
        JS_FreeValue(ctx, iter);
        JS_FreeValue(ctx, adder);
    }
    return obj;
 fail:
    if (JS_IsObject(iter)) {
        /* close the iterator object, preserving pending exception */
        JS_IteratorClose(ctx, iter, TRUE);
    }
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, adder);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* XXX: could normalize strings to speed up comparison */
static JSValueConst map_normalize_key(JSContext *ctx, JSValueConst key)
{
    uint32_t tag = JS_VALUE_GET_TAG(key);
    /* convert -0.0 to +0.0 */
    if (JS_TAG_IS_FLOAT64(tag) && JS_VALUE_GET_FLOAT64(key) == 0.0) {
        key = JS_NewInt32(ctx, 0);
    }
    return key;
}

/* XXX: better hash ? */
static uint32_t map_hash_key(JSContext *ctx, JSValueConst key)
{
    uint32_t tag = JS_VALUE_GET_NORM_TAG(key);
    uint32_t h;
    double d;
    JSFloat64Union u;

    switch(tag) {
    case JS_TAG_BOOL:
        h = JS_VALUE_GET_INT(key);
        break;
    case JS_TAG_STRING:
        h = hash_string(JS_VALUE_GET_STRING(key), 0);
        break;
    case JS_TAG_OBJECT:
    case JS_TAG_SYMBOL:
        h = (uintptr_t)JS_VALUE_GET_PTR(key) * 3163;
        break;
    case JS_TAG_INT:
        d = JS_VALUE_GET_INT(key) * 3163;
        goto hash_float64;
    case JS_TAG_FLOAT64:
        d = JS_VALUE_GET_FLOAT64(key);
        /* normalize the NaN */
        if (isnan(d))
            d = JS_FLOAT64_NAN;
    hash_float64:
        u.d = d;
        h = (u.u32[0] ^ u.u32[1]) * 3163;
        break;
    default:
        h = 0; /* XXX: bignum support */
        break;
    }
    h ^= tag;
    return h;
}

static JSMapRecord *map_find_record(JSContext *ctx, JSMapState *s,
                                    JSValueConst key)
{
    struct list_head *el;
    JSMapRecord *mr;
    uint32_t h;
    h = map_hash_key(ctx, key) & (s->hash_size - 1);
    list_for_each(el, &s->hash_table[h]) {
        mr = list_entry(el, JSMapRecord, hash_link);
        if (js_same_value_zero(ctx, mr->key, key))
            return mr;
    }
    return NULL;
}

static void map_hash_resize(JSContext *ctx, JSMapState *s)
{
    uint32_t new_hash_size, i, h;
    size_t slack;
    struct list_head *new_hash_table, *el;
    JSMapRecord *mr;

    /* XXX: no reporting of memory allocation failure */
    if (s->hash_size == 1)
        new_hash_size = 4;
    else
        new_hash_size = s->hash_size * 2;
    new_hash_table = js_realloc2(ctx, s->hash_table,
                                 sizeof(new_hash_table[0]) * new_hash_size, &slack);
    if (!new_hash_table)
        return;
    new_hash_size += slack / sizeof(*new_hash_table);

    for(i = 0; i < new_hash_size; i++)
        init_list_head(&new_hash_table[i]);

    list_for_each(el, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (!mr->empty) {
            h = map_hash_key(ctx, mr->key) & (new_hash_size - 1);
            list_add_tail(&mr->hash_link, &new_hash_table[h]);
        }
    }
    s->hash_table = new_hash_table;
    s->hash_size = new_hash_size;
    s->record_count_threshold = new_hash_size * 2;
}

static JSMapRecord *map_add_record(JSContext *ctx, JSMapState *s,
                                   JSValueConst key)
{
    uint32_t h;
    JSMapRecord *mr;

    mr = js_malloc(ctx, sizeof(*mr));
    if (!mr)
        return NULL;
    mr->ref_count = 1;
    mr->map = s;
    mr->empty = FALSE;
    if (s->is_weak) {
        JSObject *p = JS_VALUE_GET_OBJ(key);
        /* Add the weak reference */
        mr->next_weak_ref = p->first_weak_ref;
        p->first_weak_ref = mr;
    } else {
        JS_DupValue(ctx, key);
    }
    mr->key = (JSValue)key;
    h = map_hash_key(ctx, key) & (s->hash_size - 1);
    list_add_tail(&mr->hash_link, &s->hash_table[h]);
    list_add_tail(&mr->link, &s->records);
    s->record_count++;
    if (s->record_count >= s->record_count_threshold) {
        map_hash_resize(ctx, s);
    }
    return mr;
}

/* Remove the weak reference from the object weak
   reference list. we don't use a doubly linked list to
   save space, assuming a given object has few weak
       references to it */
static void delete_weak_ref(JSRuntime *rt, JSMapRecord *mr)
{
    JSMapRecord **pmr, *mr1;
    JSObject *p;

    p = JS_VALUE_GET_OBJ(mr->key);
    pmr = &p->first_weak_ref;
    for(;;) {
        mr1 = *pmr;
        assert(mr1 != NULL);
        if (mr1 == mr)
            break;
        pmr = &mr1->next_weak_ref;
    }
    *pmr = mr1->next_weak_ref;
}

static void map_delete_record(JSRuntime *rt, JSMapState *s, JSMapRecord *mr)
{
    if (mr->empty)
        return;
    list_del(&mr->hash_link);
    if (s->is_weak) {
        delete_weak_ref(rt, mr);
    } else {
        JS_FreeValueRT(rt, mr->key);
    }
    JS_FreeValueRT(rt, mr->value);
    if (--mr->ref_count == 0) {
        list_del(&mr->link);
        js_free_rt(rt, mr);
    } else {
        /* keep a zombie record for iterators */
        mr->empty = TRUE;
        mr->key = JS_UNDEFINED;
        mr->value = JS_UNDEFINED;
    }
    s->record_count--;
}

static void map_decref_record(JSRuntime *rt, JSMapRecord *mr)
{
    if (--mr->ref_count == 0) {
        /* the record can be safely removed */
        assert(mr->empty);
        list_del(&mr->link);
        js_free_rt(rt, mr);
    }
}

static void reset_weak_ref(JSRuntime *rt, JSObject *p)
{
    JSMapRecord *mr, *mr_next;
    JSMapState *s;
    
    /* first pass to remove the records from the WeakMap/WeakSet
       lists */
    for(mr = p->first_weak_ref; mr != NULL; mr = mr->next_weak_ref) {
        s = mr->map;
        assert(s->is_weak);
        assert(!mr->empty); /* no iterator on WeakMap/WeakSet */
        list_del(&mr->hash_link);
        list_del(&mr->link);
    }
    
    /* second pass to free the values to avoid modifying the weak
       reference list while traversing it. */
    for(mr = p->first_weak_ref; mr != NULL; mr = mr_next) {
        mr_next = mr->next_weak_ref;
        JS_FreeValueRT(rt, mr->value);
        js_free_rt(rt, mr);
    }

    p->first_weak_ref = NULL; /* fail safe */
}

static JSValue js_map_set(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key, value;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key(ctx, argv[0]);
    if (s->is_weak && !JS_IsObject(key))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (magic & MAGIC_SET)
        value = JS_UNDEFINED;
    else
        value = argv[1];
    mr = map_find_record(ctx, s, key);
    if (mr) {
        JS_FreeValue(ctx, mr->value);
    } else {
        mr = map_add_record(ctx, s, key);
        if (!mr)
            return JS_EXCEPTION;
    }
    mr->value = JS_DupValue(ctx, value);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_map_get(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    if (!mr)
        return JS_UNDEFINED;
    else
        return JS_DupValue(ctx, mr->value);
}

static JSValue js_map_has(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    return JS_NewBool(ctx, (mr != NULL));
}

static JSValue js_map_delete(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    if (!mr)
        return JS_FALSE;
    map_delete_record(ctx->rt, s, mr);
    return JS_TRUE;
}

static JSValue js_map_clear(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    struct list_head *el, *el1;
    JSMapRecord *mr;

    if (!s)
        return JS_EXCEPTION;
    list_for_each_safe(el, el1, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        map_delete_record(ctx->rt, s, mr);
    }
    return JS_UNDEFINED;
}

static JSValue js_map_get_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    if (!s)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, s->record_count);
}

static JSValue js_map_forEach(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSValueConst func, this_arg;
    JSValue ret, args[3];
    struct list_head *el;
    JSMapRecord *mr;

    if (!s)
        return JS_EXCEPTION;
    func = argv[0];
    if (argc > 1)
        this_arg = argv[1];
    else
        this_arg = JS_UNDEFINED;
    if (check_function(ctx, func))
        return JS_EXCEPTION;
    /* Note: the list can be modified while traversing it, but the
       current element is locked */
    el = s->records.next;
    while (el != &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (!mr->empty) {
            mr->ref_count++;
            /* must duplicate in case the record is deleted */
            args[1] = JS_DupValue(ctx, mr->key);
            if (magic)
                args[0] = args[1];
            else
                args[0] = JS_DupValue(ctx, mr->value);
            args[2] = (JSValue)this_val;
            ret = JS_Call(ctx, func, this_arg, 3, (JSValueConst *)args);
            JS_FreeValue(ctx, args[0]);
            if (!magic)
                JS_FreeValue(ctx, args[1]);
            el = el->next;
            map_decref_record(ctx->rt, mr);
            if (JS_IsException(ret))
                return ret;
            JS_FreeValue(ctx, ret);
        } else {
            el = el->next;
        }
    }
    return JS_UNDEFINED;
}

static void js_map_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p;
    JSMapState *s;
    struct list_head *el, *el1;
    JSMapRecord *mr;

    p = JS_VALUE_GET_OBJ(val);
    s = p->u.map_state;
    if (s) {
        /* if the object is deleted we are sure that no iterator is
           using it */
        list_for_each_safe(el, el1, &s->records) {
            mr = list_entry(el, JSMapRecord, link);
            if (!mr->empty) {
                if (s->is_weak)
                    delete_weak_ref(rt, mr);
                else
                    JS_FreeValueRT(rt, mr->key);
                JS_FreeValueRT(rt, mr->value);
            }
            js_free_rt(rt, mr);
        }
        js_free_rt(rt, s->hash_table);
        js_free_rt(rt, s);
    }
}

static void js_map_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSMapState *s;
    struct list_head *el;
    JSMapRecord *mr;

    s = p->u.map_state;
    if (s) {
        list_for_each(el, &s->records) {
            mr = list_entry(el, JSMapRecord, link);
            if (!s->is_weak)
                JS_MarkValue(rt, mr->key, mark_func);
            JS_MarkValue(rt, mr->value, mark_func);
        }
    }
}

/* Map Iterator */

typedef struct JSMapIteratorData {
    JSValue obj;
    JSIteratorKindEnum kind;
    JSMapRecord *cur_record;
} JSMapIteratorData;

static void js_map_iterator_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p;
    JSMapIteratorData *it;

    p = JS_VALUE_GET_OBJ(val);
    it = p->u.map_iterator_data;
    if (it) {
        /* During the GC sweep phase the Map finalizer may be
           called before the Map iterator finalizer */
        if (JS_IsLiveObject(rt, it->obj) && it->cur_record) {
            map_decref_record(rt, it->cur_record);
        }
        JS_FreeValueRT(rt, it->obj);
        js_free_rt(rt, it);
    }
}

static void js_map_iterator_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSMapIteratorData *it;
    it = p->u.map_iterator_data;
    if (it) {
        /* the record is already marked by the object */
        JS_MarkValue(rt, it->obj, mark_func);
    }
}

static JSValue js_create_map_iterator(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    JSIteratorKindEnum kind;
    JSMapState *s;
    JSMapIteratorData *it;
    JSValue enum_obj;

    kind = magic >> 2;
    magic &= 3;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    if (!s)
        return JS_EXCEPTION;
    enum_obj = JS_NewObjectClass(ctx, JS_CLASS_MAP_ITERATOR + magic);
    if (JS_IsException(enum_obj))
        goto fail;
    it = js_malloc(ctx, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, enum_obj);
        goto fail;
    }
    it->obj = JS_DupValue(ctx, this_val);
    it->kind = kind;
    it->cur_record = NULL;
    JS_SetOpaque(enum_obj, it);
    return enum_obj;
 fail:
    return JS_EXCEPTION;
}

static JSValue js_map_iterator_next(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    BOOL *pdone, int magic)
{
    JSMapIteratorData *it;
    JSMapState *s;
    JSMapRecord *mr;
    struct list_head *el;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP_ITERATOR + magic);
    if (!it) {
        *pdone = FALSE;
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(it->obj))
        goto done;
    s = JS_GetOpaque(it->obj, JS_CLASS_MAP + magic);
    assert(s != NULL);
    if (!it->cur_record) {
        el = s->records.next;
    } else {
        mr = it->cur_record;
        el = mr->link.next;
        map_decref_record(ctx->rt, mr); /* the record can be freed here */
    }
    for(;;) {
        if (el == &s->records) {
            /* no more record  */
            it->cur_record = NULL;
            JS_FreeValue(ctx, it->obj);
            it->obj = JS_UNDEFINED;
        done:
            /* end of enumeration */
            *pdone = TRUE;
            return JS_UNDEFINED;
        }
        mr = list_entry(el, JSMapRecord, link);
        if (!mr->empty)
            break;
        /* get the next record */
        el = mr->link.next;
    }

    /* lock the record so that it won't be freed */
    mr->ref_count++;
    it->cur_record = mr;
    *pdone = FALSE;

    if (it->kind == JS_ITERATOR_KIND_KEY) {
        return JS_DupValue(ctx, mr->key);
    } else {
        JSValueConst args[2];
        args[0] = mr->key;
        if (magic)
            args[1] = mr->key;
        else
            args[1] = mr->value;
        if (it->kind == JS_ITERATOR_KIND_VALUE) {
            return JS_DupValue(ctx, args[1]);
        } else {
            return js_create_array(ctx, 2, args);
        }
    }
}

static const JSCFunctionListEntry js_map_funcs[] = {
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_map_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, js_map_set, 0 ),
    JS_CFUNC_MAGIC_DEF("get", 1, js_map_get, 0 ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, 0 ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, 0 ),
    JS_CFUNC_MAGIC_DEF("clear", 0, js_map_clear, 0 ),
    JS_CGETSET_MAGIC_DEF("size", js_map_get_size, NULL, 0),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_map_forEach, 0 ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_map_iterator, (JS_ITERATOR_KIND_VALUE << 2) | 0 ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY << 2) | 0 ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY_AND_VALUE << 2) | 0 ),
    JS_ALIAS_DEF("[Symbol.iterator]", "entries" ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Map", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_map_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_map_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Map Iterator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_set_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, js_map_set, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("clear", 0, js_map_clear, MAGIC_SET ),
    JS_CGETSET_MAGIC_DEF("size", js_map_get_size, NULL, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_map_forEach, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY << 2) | MAGIC_SET ),
    JS_ALIAS_DEF("keys", "values" ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY_AND_VALUE << 2) | MAGIC_SET ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Set", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_set_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_map_iterator_next, MAGIC_SET ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Set Iterator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_weak_map_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, js_map_set, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("get", 1, js_map_get, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_WEAK ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WeakMap", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_weak_set_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, js_map_set, MAGIC_SET | MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_SET | MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_SET | MAGIC_WEAK ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WeakSet", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry * const js_map_proto_funcs_ptr[6] = {
    js_map_proto_funcs,
    js_set_proto_funcs,
    js_weak_map_proto_funcs,
    js_weak_set_proto_funcs,
    js_map_iterator_proto_funcs,
    js_set_iterator_proto_funcs,
};

static const uint8_t js_map_proto_funcs_count[6] = {
    countof(js_map_proto_funcs),
    countof(js_set_proto_funcs),
    countof(js_weak_map_proto_funcs),
    countof(js_weak_set_proto_funcs),
    countof(js_map_iterator_proto_funcs),
    countof(js_set_iterator_proto_funcs),
};

void JS_AddIntrinsicMapSet(JSContext *ctx)
{
    int i;
    JSValue obj1;
    char buf[ATOM_GET_STR_BUF_SIZE];

    for(i = 0; i < 4; i++) {
        const char *name = JS_AtomGetStr(ctx, buf, sizeof(buf),
                                         JS_ATOM_Map + i);
        ctx->class_proto[JS_CLASS_MAP + i] = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_MAP + i],
                                   js_map_proto_funcs_ptr[i],
                                   js_map_proto_funcs_count[i]);
        obj1 = JS_NewCFunctionMagic(ctx, js_map_constructor, name, 0,
                                    JS_CFUNC_constructor_magic, i);
        if (i < 2) {
            JS_SetPropertyFunctionList(ctx, obj1, js_map_funcs,
                                       countof(js_map_funcs));
        }
        JS_NewGlobalCConstructor2(ctx, obj1, name, ctx->class_proto[JS_CLASS_MAP + i]);
    }

    for(i = 0; i < 2; i++) {
        ctx->class_proto[JS_CLASS_MAP_ITERATOR + i] =
            JS_NewObjectProto(ctx, ctx->iterator_proto);
        JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_MAP_ITERATOR + i],
                                   js_map_proto_funcs_ptr[i + 4],
                                   js_map_proto_funcs_count[i + 4]);
    }
}

/* Generator */
static const JSCFunctionListEntry js_generator_function_proto_funcs[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "GeneratorFunction", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry js_generator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 1, js_generator_next, GEN_MAGIC_NEXT ),
    JS_ITERATOR_NEXT_DEF("return", 1, js_generator_next, GEN_MAGIC_RETURN ),
    JS_ITERATOR_NEXT_DEF("throw", 1, js_generator_next, GEN_MAGIC_THROW ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Generator", JS_PROP_CONFIGURABLE),
};

/* Promise */

typedef enum JSPromiseStateEnum {
    JS_PROMISE_PENDING,
    JS_PROMISE_FULFILLED,
    JS_PROMISE_REJECTED,
} JSPromiseStateEnum;

typedef struct JSPromiseData {
    JSPromiseStateEnum promise_state;
    /* 0=fulfill, 1=reject, list of JSPromiseReactionData.link */
    struct list_head promise_reactions[2];
    BOOL is_handled; /* Note: only useful to debug */
    JSValue promise_result;
} JSPromiseData;

typedef struct JSPromiseFunctionDataResolved {
    int ref_count;
    BOOL already_resolved;
} JSPromiseFunctionDataResolved;

typedef struct JSPromiseFunctionData {
    JSValue promise;
    JSPromiseFunctionDataResolved *presolved;
} JSPromiseFunctionData;

typedef struct JSPromiseReactionData {
    struct list_head link; /* not used in promise_reaction_job */
    JSValue resolving_funcs[2];
    JSValue handler;
} JSPromiseReactionData;

static int js_create_resolving_functions(JSContext *ctx, JSValue *args,
                                         JSValueConst promise);

static void promise_reaction_data_free(JSRuntime *rt,
                                       JSPromiseReactionData *rd)
{
    JS_FreeValueRT(rt, rd->resolving_funcs[0]);
    JS_FreeValueRT(rt, rd->resolving_funcs[1]);
    JS_FreeValueRT(rt, rd->handler);
    js_free_rt(rt, rd);
}

static JSValue promise_reaction_job(JSContext *ctx, int argc,
                                    JSValueConst *argv)
{
    JSValueConst handler, arg, func;
    JSValue res, res2;
    BOOL is_reject;

    assert(argc == 5);
    handler = argv[2];
    is_reject = JS_ToBool(ctx, argv[3]);
    arg = argv[4];
#ifdef DUMP_PROMISE
    printf("promise_reaction_job: is_reject=%d\n", is_reject);
#endif

    if (JS_IsUndefined(handler)) {
        if (is_reject) {
            res = JS_Throw(ctx, JS_DupValue(ctx, arg));
        } else {
            res = JS_DupValue(ctx, arg);
        }
    } else {
        res = JS_Call(ctx, handler, JS_UNDEFINED, 1, &arg);
    }
    is_reject = JS_IsException(res);
    if (is_reject)
        res = JS_GetException(ctx);
    func = argv[is_reject];
    /* as an extension, we support undefined as value to avoid
       creating a dummy promise in the 'await' implementation of async
       functions */
    if (!JS_IsUndefined(func)) {
        res2 = JS_Call(ctx, func, JS_UNDEFINED,
                       1, (JSValueConst *)&res);
    } else {
        res2 = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, res);

    return res2;
}

void JS_SetHostPromiseRejectionTracker(JSRuntime *rt,
                                       JSHostPromiseRejectionTracker *cb,
                                       void *opaque)
{
    rt->host_promise_rejection_tracker = cb;
    rt->host_promise_rejection_tracker_opaque = opaque;
}

static void fulfill_or_reject_promise(JSContext *ctx, JSValueConst promise,
                                      JSValueConst value, BOOL is_reject)
{
    JSPromiseData *s = JS_GetOpaque(promise, JS_CLASS_PROMISE);
    struct list_head *el, *el1;
    JSPromiseReactionData *rd;
    JSValueConst args[5];

    if (!s || s->promise_state != JS_PROMISE_PENDING)
        return; /* should never happen */
    set_value(ctx, &s->promise_result, JS_DupValue(ctx, value));
    s->promise_state = JS_PROMISE_FULFILLED + is_reject;
#ifdef DUMP_PROMISE
    printf("fulfill_or_reject_promise: is_reject=%d\n", is_reject);
#endif
    if (s->promise_state == JS_PROMISE_REJECTED && !s->is_handled) {
        JSRuntime *rt = ctx->rt;
        if (rt->host_promise_rejection_tracker) {
            rt->host_promise_rejection_tracker(ctx, promise, value, FALSE,
                                               rt->host_promise_rejection_tracker_opaque);
        }
    }

    list_for_each_safe(el, el1, &s->promise_reactions[is_reject]) {
        rd = list_entry(el, JSPromiseReactionData, link);
        args[0] = rd->resolving_funcs[0];
        args[1] = rd->resolving_funcs[1];
        args[2] = rd->handler;
        args[3] = JS_NewBool(ctx, is_reject);
        args[4] = value;
        JS_EnqueueJob(ctx, promise_reaction_job, 5, args);
        list_del(&rd->link);
        promise_reaction_data_free(ctx->rt, rd);
    }

    list_for_each_safe(el, el1, &s->promise_reactions[1 - is_reject]) {
        rd = list_entry(el, JSPromiseReactionData, link);
        list_del(&rd->link);
        promise_reaction_data_free(ctx->rt, rd);
    }
}

static void reject_promise(JSContext *ctx, JSValueConst promise,
                           JSValueConst value)
{
    fulfill_or_reject_promise(ctx, promise, value, TRUE);
}

static JSValue js_promise_resolve_thenable_job(JSContext *ctx,
                                               int argc, JSValueConst *argv)
{
    JSValueConst promise, thenable, then;
    JSValue args[2], res;

#ifdef DUMP_PROMISE
    printf("js_promise_resolve_thenable_job\n");
#endif
    assert(argc == 3);
    promise = argv[0];
    thenable = argv[1];
    then = argv[2];
    if (js_create_resolving_functions(ctx, args, promise) < 0)
        return JS_EXCEPTION;
    res = JS_Call(ctx, then, thenable, 2, (JSValueConst *)args);
    if (JS_IsException(res)) {
        JSValue error = JS_GetException(ctx);
        res = JS_Call(ctx, args[1], JS_UNDEFINED, 1, (JSValueConst *)&error);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return res;
}

static void js_promise_resolve_function_free_resolved(JSRuntime *rt,
                                                      JSPromiseFunctionDataResolved *sr)
{
    if (--sr->ref_count == 0) {
        js_free_rt(rt, sr);
    }
}

static int js_create_resolving_functions(JSContext *ctx,
                                         JSValue *resolving_funcs,
                                         JSValueConst promise)

{
    JSValue obj;
    JSPromiseFunctionData *s;
    JSPromiseFunctionDataResolved *sr;
    int i, ret;

    sr = js_malloc(ctx, sizeof(*sr));
    if (!sr)
        return -1;
    sr->ref_count = 1;
    sr->already_resolved = FALSE; /* must be shared between the two functions */
    ret = 0;
    for(i = 0; i < 2; i++) {
        obj = JS_NewObjectProtoClass(ctx, ctx->function_proto,
                                     JS_CLASS_PROMISE_RESOLVE_FUNCTION + i);
        if (JS_IsException(obj))
            goto fail;
        s = js_malloc(ctx, sizeof(*s));
        if (!s) {
            JS_FreeValue(ctx, obj);
        fail:

            if (i != 0)
                JS_FreeValue(ctx, resolving_funcs[0]);
            ret = -1;
            break;
        }
        sr->ref_count++;
        s->presolved = sr;
        s->promise = JS_DupValue(ctx, promise);
        JS_SetOpaque(obj, s);
        js_function_set_properties(ctx, obj, JS_ATOM_empty_string, 1);
        resolving_funcs[i] = obj;
    }
    js_promise_resolve_function_free_resolved(ctx->rt, sr);
    return ret;
}

static void js_promise_resolve_function_finalizer(JSRuntime *rt, JSValue val)
{
    JSPromiseFunctionData *s = JS_VALUE_GET_OBJ(val)->u.promise_function_data;
    if (s) {
        js_promise_resolve_function_free_resolved(rt, s->presolved);
        JS_FreeValueRT(rt, s->promise);
        js_free_rt(rt, s);
    }
}

static void js_promise_resolve_function_mark(JSRuntime *rt, JSValueConst val,
                                             JS_MarkFunc *mark_func)
{
    JSPromiseFunctionData *s = JS_VALUE_GET_OBJ(val)->u.promise_function_data;
    if (s) {
        JS_MarkValue(rt, s->promise, mark_func);
    }
}

static JSValue js_promise_resolve_function_call(JSContext *ctx,
                                                JSValueConst func_obj,
                                                JSValueConst this_val,
                                                int argc, JSValueConst *argv,
                                                int flags)
{
    JSObject *p = JS_VALUE_GET_OBJ(func_obj);
    JSPromiseFunctionData *s;
    JSValueConst resolution, args[3];
    JSValue then;
    BOOL is_reject;

    s = p->u.promise_function_data;
    if (!s || s->presolved->already_resolved)
        return JS_UNDEFINED;
    s->presolved->already_resolved = TRUE;
    is_reject = p->class_id - JS_CLASS_PROMISE_RESOLVE_FUNCTION;
    if (argc > 0)
        resolution = argv[0];
    else
        resolution = JS_UNDEFINED;
#ifdef DUMP_PROMISE
    printf("js_promise_resolving_function_call: is_reject=%d resolution=", is_reject);
    JS_DumpValue(ctx, resolution);
    printf("\n");
#endif
    if (is_reject || !JS_IsObject(resolution)) {
        goto done;
    } else if (js_same_value(ctx, resolution, s->promise)) {
        JS_ThrowTypeError(ctx, "promise self resolution");
        goto fail_reject;
    }
    then = JS_GetProperty(ctx, resolution, JS_ATOM_then);
    if (JS_IsException(then)) {
        JSValue error;
    fail_reject:
        error = JS_GetException(ctx);
        reject_promise(ctx, s->promise, error);
        JS_FreeValue(ctx, error);
    } else if (!JS_IsFunction(ctx, then)) {
        JS_FreeValue(ctx, then);
    done:
        fulfill_or_reject_promise(ctx, s->promise, resolution, is_reject);
    } else {
        args[0] = s->promise;
        args[1] = resolution;
        args[2] = then;
        JS_EnqueueJob(ctx, js_promise_resolve_thenable_job, 3, args);
        JS_FreeValue(ctx, then);
    }
    return JS_UNDEFINED;
}

static void js_promise_finalizer(JSRuntime *rt, JSValue val)
{
    JSPromiseData *s = JS_GetOpaque(val, JS_CLASS_PROMISE);
    struct list_head *el, *el1;
    int i;

    if (!s)
        return;
    for(i = 0; i < 2; i++) {
        list_for_each_safe(el, el1, &s->promise_reactions[i]) {
            JSPromiseReactionData *rd =
                list_entry(el, JSPromiseReactionData, link);
            promise_reaction_data_free(rt, rd);
        }
    }
    JS_FreeValueRT(rt, s->promise_result);
    js_free_rt(rt, s);
}

static void js_promise_mark(JSRuntime *rt, JSValueConst val,
                            JS_MarkFunc *mark_func)
{
    JSPromiseData *s = JS_GetOpaque(val, JS_CLASS_PROMISE);
    struct list_head *el;
    int i;

    if (!s)
        return;
    for(i = 0; i < 2; i++) {
        list_for_each(el, &s->promise_reactions[i]) {
            JSPromiseReactionData *rd =
                list_entry(el, JSPromiseReactionData, link);
            JS_MarkValue(rt, rd->resolving_funcs[0], mark_func);
            JS_MarkValue(rt, rd->resolving_funcs[1], mark_func);
            JS_MarkValue(rt, rd->handler, mark_func);
        }
    }
    JS_MarkValue(rt, s->promise_result, mark_func);
}

static JSValue js_promise_constructor(JSContext *ctx, JSValueConst new_target,
                                      int argc, JSValueConst *argv)
{
    JSValueConst executor;
    JSValue obj;
    JSPromiseData *s;
    JSValue args[2], ret;
    int i;

    executor = argv[0];
    if (check_function(ctx, executor))
        return JS_EXCEPTION;
    obj = js_create_from_ctor(ctx, new_target, JS_CLASS_PROMISE);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        goto fail;
    s->promise_state = JS_PROMISE_PENDING;
    s->is_handled = FALSE;
    for(i = 0; i < 2; i++)
        init_list_head(&s->promise_reactions[i]);
    s->promise_result = JS_UNDEFINED;
    JS_SetOpaque(obj, s);
    if (js_create_resolving_functions(ctx, args, obj))
        goto fail;
    ret = JS_Call(ctx, executor, JS_UNDEFINED, 2, (JSValueConst *)args);
    if (JS_IsException(ret)) {
        JSValue ret2, error;
        error = JS_GetException(ctx);
        ret2 = JS_Call(ctx, args[1], JS_UNDEFINED, 1, (JSValueConst *)&error);
        JS_FreeValue(ctx, error);
        if (JS_IsException(ret2))
            goto fail1;
        JS_FreeValue(ctx, ret2);
    }
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return obj;
 fail1:
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_promise_executor(JSContext *ctx,
                                   JSValueConst this_val,
                                   int argc, JSValueConst *argv,
                                   int magic, JSValue *func_data)
{
    int i;

    for(i = 0; i < 2; i++) {
        if (!JS_IsUndefined(func_data[i]))
            return JS_ThrowTypeError(ctx, "resolving function already set");
        func_data[i] = JS_DupValue(ctx, argv[i]);
    }
    return JS_UNDEFINED;
}

static JSValue js_promise_executor_new(JSContext *ctx)
{
    JSValueConst func_data[2];

    func_data[0] = JS_UNDEFINED;
    func_data[1] = JS_UNDEFINED;
    return JS_NewCFunctionData(ctx, js_promise_executor, 2,
                               0, 2, func_data);
}

static JSValue js_new_promise_capability(JSContext *ctx,
                                         JSValue *resolving_funcs,
                                         JSValueConst ctor)
{
    JSValue executor, result_promise;
    JSCFunctionDataRecord *s;
    int i;

    executor = js_promise_executor_new(ctx);
    if (JS_IsException(executor))
        return executor;

    if (JS_IsUndefined(ctor)) {
        result_promise = js_promise_constructor(ctx, ctor, 1,
                                                (JSValueConst *)&executor);
    } else {
        result_promise = JS_CallConstructor(ctx, ctor, 1,
                                            (JSValueConst *)&executor);
    }
    if (JS_IsException(result_promise))
        goto fail;
    s = JS_GetOpaque(executor, JS_CLASS_C_FUNCTION_DATA);
    for(i = 0; i < 2; i++) {
        if (check_function(ctx, s->data[i]))
            goto fail;
    }
    for(i = 0; i < 2; i++)
        resolving_funcs[i] = JS_DupValue(ctx, s->data[i]);
    JS_FreeValue(ctx, executor);
    return result_promise;
 fail:
    JS_FreeValue(ctx, executor);
    JS_FreeValue(ctx, result_promise);
    return JS_EXCEPTION;
}

JSValue JS_NewPromiseCapability(JSContext *ctx, JSValue *resolving_funcs)
{
    return js_new_promise_capability(ctx, resolving_funcs, JS_UNDEFINED);
}

static JSValue js_promise_resolve(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic)
{
    JSValue result_promise, resolving_funcs[2], ret;
    BOOL is_reject = magic;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    if (!is_reject && JS_GetOpaque(argv[0], JS_CLASS_PROMISE)) {
        JSValue ctor;
        BOOL is_same;
        ctor = JS_GetProperty(ctx, argv[0], JS_ATOM_constructor);
        if (JS_IsException(ctor))
            return ctor;
        is_same = js_same_value(ctx, ctor, this_val);
        JS_FreeValue(ctx, ctor);
        if (is_same)
            return JS_DupValue(ctx, argv[0]);
    }
    result_promise = js_new_promise_capability(ctx, resolving_funcs, this_val);
    if (JS_IsException(result_promise))
        return result_promise;
    ret = JS_Call(ctx, resolving_funcs[is_reject], JS_UNDEFINED, 1, argv);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    if (JS_IsException(ret)) {
        JS_FreeValue(ctx, result_promise);
        return ret;
    }
    JS_FreeValue(ctx, ret);
    return result_promise;
}

#if 0
static JSValue js_promise___newPromiseCapability(JSContext *ctx,
                                                 JSValueConst this_val,
                                                 int argc, JSValueConst *argv)
{
    JSValue result_promise, resolving_funcs[2], obj;
    JSValueConst ctor;
    ctor = argv[0];
    if (!JS_IsObject(ctor))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    result_promise = js_new_promise_capability(ctx, resolving_funcs, ctor);
    if (JS_IsException(result_promise))
        return result_promise;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, result_promise);
        return JS_EXCEPTION;
    }
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_promise, result_promise, JS_PROP_C_W_E);
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_resolve, resolving_funcs[0], JS_PROP_C_W_E);
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_reject, resolving_funcs[1], JS_PROP_C_W_E);
    return obj;
}
#endif

static __exception int remainingElementsCount_add(JSContext *ctx,
                                                  JSValueConst resolve_element_env,
                                                  int addend)
{
    JSValue val;
    int remainingElementsCount;

    val = JS_GetPropertyUint32(ctx, resolve_element_env, 0);
    if (JS_IsException(val))
        return -1;
    if (JS_ToInt32Free(ctx, &remainingElementsCount, val))
        return -1;
    remainingElementsCount += addend;
    if (JS_SetPropertyUint32(ctx, resolve_element_env, 0,
                             JS_NewInt32(ctx, remainingElementsCount)) < 0)
        return -1;
    return (remainingElementsCount == 0);
}

#define PROMISE_MAGIC_all        0
#define PROMISE_MAGIC_allSettled 1
#define PROMISE_MAGIC_any        2

static JSValue js_promise_all_resolve_element(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              int magic,
                                              JSValue *func_data)
{
    int resolve_type = magic & 3;
    int is_reject = magic & 4;
    BOOL alreadyCalled = JS_ToBool(ctx, func_data[0]);
    JSValueConst values = func_data[2];
    JSValueConst resolve = func_data[3];
    JSValueConst resolve_element_env = func_data[4];
    JSValue ret, obj;
    int is_zero, index;
    
    if (JS_ToInt32(ctx, &index, func_data[1]))
        return JS_EXCEPTION;
    if (alreadyCalled)
        return JS_UNDEFINED;
    func_data[0] = JS_NewBool(ctx, TRUE);

    if (resolve_type == PROMISE_MAGIC_allSettled) {
        JSValue str;
        
        obj = JS_NewObject(ctx);
        if (JS_IsException(obj))
            return JS_EXCEPTION;
        str = JS_NewString(ctx, is_reject ? "rejected" : "fulfilled");
        if (JS_IsException(str))
            goto fail1;
        if (JS_DefinePropertyValue(ctx, obj, JS_ATOM_status,
                                   str,
                                   JS_PROP_C_W_E) < 0)
            goto fail1;
        if (JS_DefinePropertyValue(ctx, obj,
                                   is_reject ? JS_ATOM_reason : JS_ATOM_value,
                                   JS_DupValue(ctx, argv[0]),
                                   JS_PROP_C_W_E) < 0) {
        fail1:
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    } else {
        obj = JS_DupValue(ctx, argv[0]);
    }
    if (JS_DefinePropertyValueUint32(ctx, values, index,
                                     obj, JS_PROP_C_W_E) < 0)
        return JS_EXCEPTION;
    
    is_zero = remainingElementsCount_add(ctx, resolve_element_env, -1);
    if (is_zero < 0)
        return JS_EXCEPTION;
    if (is_zero) {
        if (resolve_type == PROMISE_MAGIC_any) {
            JSValue error;
            error = js_aggregate_error_constructor(ctx, values);
            if (JS_IsException(error))
                return JS_EXCEPTION;
            ret = JS_Call(ctx, resolve, JS_UNDEFINED, 1, (JSValueConst *)&error);
            JS_FreeValue(ctx, error);
        } else {
            ret = JS_Call(ctx, resolve, JS_UNDEFINED, 1, (JSValueConst *)&values);
        }
        if (JS_IsException(ret))
            return ret;
        JS_FreeValue(ctx, ret);
    }
    return JS_UNDEFINED;
}

/* magic = 0: Promise.all 1: Promise.allSettled */
static JSValue js_promise_all(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    JSValue result_promise, resolving_funcs[2], item, next_promise, ret;
    JSValue next_method = JS_UNDEFINED, values = JS_UNDEFINED;
    JSValue resolve_element_env = JS_UNDEFINED, resolve_element, reject_element;
    JSValue promise_resolve = JS_UNDEFINED, iter = JS_UNDEFINED;
    JSValueConst then_args[2], resolve_element_data[5];
    BOOL done;
    int index, is_zero, is_promise_any = (magic == PROMISE_MAGIC_any);
    
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    result_promise = js_new_promise_capability(ctx, resolving_funcs, this_val);
    if (JS_IsException(result_promise))
        return result_promise;
    promise_resolve = JS_GetProperty(ctx, this_val, JS_ATOM_resolve);
    if (JS_IsException(promise_resolve) ||
        check_function(ctx, promise_resolve))
        goto fail_reject;
    iter = JS_GetIterator(ctx, argv[0], FALSE);
    if (JS_IsException(iter)) {
        JSValue error;
    fail_reject:
        error = JS_GetException(ctx);
        ret = JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1,
                       (JSValueConst *)&error);
        JS_FreeValue(ctx, error);
        if (JS_IsException(ret))
            goto fail;
        JS_FreeValue(ctx, ret);
    } else {
        next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next_method))
            goto fail_reject;
        values = JS_NewArray(ctx);
        if (JS_IsException(values))
            goto fail_reject;
        resolve_element_env = JS_NewArray(ctx);
        if (JS_IsException(resolve_element_env))
            goto fail_reject;
        /* remainingElementsCount field */
        if (JS_DefinePropertyValueUint32(ctx, resolve_element_env, 0,
                                         JS_NewInt32(ctx, 1),
                                         JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_WRITABLE) < 0)
            goto fail_reject;
        
        index = 0;
        for(;;) {
            /* XXX: conformance: should close the iterator if error on 'done'
               access, but not on 'value' access */
            item = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
            if (JS_IsException(item))
                goto fail_reject;
            if (done)
                break;
            next_promise = JS_Call(ctx, promise_resolve, 
                                   this_val, 1, (JSValueConst *)&item);
            JS_FreeValue(ctx, item);
            if (JS_IsException(next_promise)) {
            fail_reject1:
                JS_IteratorClose(ctx, iter, TRUE);
                goto fail_reject;
            }
            resolve_element_data[0] = JS_NewBool(ctx, FALSE);
            resolve_element_data[1] = (JSValueConst)JS_NewInt32(ctx, index);
            resolve_element_data[2] = values;
            resolve_element_data[3] = resolving_funcs[is_promise_any];
            resolve_element_data[4] = resolve_element_env;
            resolve_element =
                JS_NewCFunctionData(ctx, js_promise_all_resolve_element, 1,
                                    magic, 5, resolve_element_data);
            if (JS_IsException(resolve_element)) {
                JS_FreeValue(ctx, next_promise);
                goto fail_reject1;
            }
            
            if (magic == PROMISE_MAGIC_allSettled) {
                reject_element =
                    JS_NewCFunctionData(ctx, js_promise_all_resolve_element, 1,
                                        magic | 4, 5, resolve_element_data);
                if (JS_IsException(reject_element)) {
                    JS_FreeValue(ctx, next_promise);
                    goto fail_reject1;
                }
            } else if (magic == PROMISE_MAGIC_any) {
                if (JS_DefinePropertyValueUint32(ctx, values, index,
                                                 JS_UNDEFINED, JS_PROP_C_W_E) < 0)
                    goto fail_reject1;
                reject_element = resolve_element;
                resolve_element = JS_DupValue(ctx, resolving_funcs[0]);
            } else {
                reject_element = JS_DupValue(ctx, resolving_funcs[1]);
            }

            if (remainingElementsCount_add(ctx, resolve_element_env, 1) < 0) {
                JS_FreeValue(ctx, next_promise);
                JS_FreeValue(ctx, resolve_element);
                JS_FreeValue(ctx, reject_element);
                goto fail_reject1;
            }

            then_args[0] = resolve_element;
            then_args[1] = reject_element;
            ret = JS_InvokeFree(ctx, next_promise, JS_ATOM_then, 2, then_args);
            JS_FreeValue(ctx, resolve_element);
            JS_FreeValue(ctx, reject_element);
            if (check_exception_free(ctx, ret))
                goto fail_reject1;
            index++;
        }

        is_zero = remainingElementsCount_add(ctx, resolve_element_env, -1);
        if (is_zero < 0)
            goto fail_reject;
        if (is_zero) {
            if (magic == PROMISE_MAGIC_any) {
                JSValue error;
                error = js_aggregate_error_constructor(ctx, values);
                if (JS_IsException(error))
                    goto fail_reject;
                JS_FreeValue(ctx, values);
                values = error;
            }
            ret = JS_Call(ctx, resolving_funcs[is_promise_any], JS_UNDEFINED,
                          1, (JSValueConst *)&values);
            if (check_exception_free(ctx, ret))
                goto fail_reject;
        }
    }
 done:
    JS_FreeValue(ctx, promise_resolve);
    JS_FreeValue(ctx, resolve_element_env);
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return result_promise;
 fail:
    JS_FreeValue(ctx, result_promise);
    result_promise = JS_EXCEPTION;
    goto done;
}

static JSValue js_promise_race(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSValue result_promise, resolving_funcs[2], item, next_promise, ret;
    JSValue next_method = JS_UNDEFINED, iter = JS_UNDEFINED;
    JSValue promise_resolve = JS_UNDEFINED;
    BOOL done;

    if (!JS_IsObject(this_val))
        return JS_ThrowTypeErrorNotAnObject(ctx);
    result_promise = js_new_promise_capability(ctx, resolving_funcs, this_val);
    if (JS_IsException(result_promise))
        return result_promise;
    promise_resolve = JS_GetProperty(ctx, this_val, JS_ATOM_resolve);
    if (JS_IsException(promise_resolve) ||
        check_function(ctx, promise_resolve))
        goto fail_reject;
    iter = JS_GetIterator(ctx, argv[0], FALSE);
    if (JS_IsException(iter)) {
        JSValue error;
    fail_reject:
        error = JS_GetException(ctx);
        ret = JS_Call(ctx, resolving_funcs[1], JS_UNDEFINED, 1,
                       (JSValueConst *)&error);
        JS_FreeValue(ctx, error);
        if (JS_IsException(ret))
            goto fail;
        JS_FreeValue(ctx, ret);
    } else {
        next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
        if (JS_IsException(next_method))
            goto fail_reject;

        for(;;) {
            /* XXX: conformance: should close the iterator if error on 'done'
               access, but not on 'value' access */
            item = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
            if (JS_IsException(item))
                goto fail_reject;
            if (done)
                break;
            next_promise = JS_Call(ctx, promise_resolve,
                                   this_val, 1, (JSValueConst *)&item);
            JS_FreeValue(ctx, item);
            if (JS_IsException(next_promise)) {
            fail_reject1:
                JS_IteratorClose(ctx, iter, TRUE);
                goto fail_reject;
            }
            ret = JS_InvokeFree(ctx, next_promise, JS_ATOM_then, 2,
                                (JSValueConst *)resolving_funcs);
            if (check_exception_free(ctx, ret))
                goto fail_reject1;
        }
    }
 done:
    JS_FreeValue(ctx, promise_resolve);
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    return result_promise;
 fail:
    //JS_FreeValue(ctx, next_method); // why not???
    JS_FreeValue(ctx, result_promise);
    result_promise = JS_EXCEPTION;
    goto done;
}

static __exception int perform_promise_then(JSContext *ctx,
                                            JSValueConst promise,
                                            JSValueConst *resolve_reject,
                                            JSValueConst *cap_resolving_funcs)
{
    JSPromiseData *s = JS_GetOpaque(promise, JS_CLASS_PROMISE);
    JSPromiseReactionData *rd_array[2], *rd;
    int i, j;

    rd_array[0] = NULL;
    rd_array[1] = NULL;
    for(i = 0; i < 2; i++) {
        JSValueConst handler;
        rd = js_mallocz(ctx, sizeof(*rd));
        if (!rd) {
            if (i == 1)
                promise_reaction_data_free(ctx->rt, rd_array[0]);
            return -1;
        }
        for(j = 0; j < 2; j++)
            rd->resolving_funcs[j] = JS_DupValue(ctx, cap_resolving_funcs[j]);
        handler = resolve_reject[i];
        if (!JS_IsFunction(ctx, handler))
            handler = JS_UNDEFINED;
        rd->handler = JS_DupValue(ctx, handler);
        rd_array[i] = rd;
    }

    if (s->promise_state == JS_PROMISE_PENDING) {
        for(i = 0; i < 2; i++)
            list_add_tail(&rd_array[i]->link, &s->promise_reactions[i]);
    } else {
        JSValueConst args[5];
        if (s->promise_state == JS_PROMISE_REJECTED && !s->is_handled) {
            JSRuntime *rt = ctx->rt;
            if (rt->host_promise_rejection_tracker) {
                rt->host_promise_rejection_tracker(ctx, promise, s->promise_result,
                                                   TRUE, rt->host_promise_rejection_tracker_opaque);
            }
        }
        i = s->promise_state - JS_PROMISE_FULFILLED;
        rd = rd_array[i];
        args[0] = rd->resolving_funcs[0];
        args[1] = rd->resolving_funcs[1];
        args[2] = rd->handler;
        args[3] = JS_NewBool(ctx, i);
        args[4] = s->promise_result;
        JS_EnqueueJob(ctx, promise_reaction_job, 5, args);
        for(i = 0; i < 2; i++)
            promise_reaction_data_free(ctx->rt, rd_array[i]);
    }
    s->is_handled = TRUE;
    return 0;
}

static JSValue js_promise_then(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JSValue ctor, result_promise, resolving_funcs[2];
    JSPromiseData *s;
    int i, ret;

    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_PROMISE);
    if (!s)
        return JS_EXCEPTION;

    ctor = JS_SpeciesConstructor(ctx, this_val, JS_UNDEFINED);
    if (JS_IsException(ctor))
        return ctor;
    result_promise = js_new_promise_capability(ctx, resolving_funcs, ctor);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(result_promise))
        return result_promise;
    ret = perform_promise_then(ctx, this_val, argv,
                               (JSValueConst *)resolving_funcs);
    for(i = 0; i < 2; i++)
        JS_FreeValue(ctx, resolving_funcs[i]);
    if (ret) {
        JS_FreeValue(ctx, result_promise);
        return JS_EXCEPTION;
    }
    return result_promise;
}

static JSValue js_promise_catch(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValueConst args[2];
    args[0] = JS_UNDEFINED;
    args[1] = argv[0];
    return JS_Invoke(ctx, this_val, JS_ATOM_then, 2, args);
}

static JSValue js_promise_finally_value_thunk(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv,
                                              int magic, JSValue *func_data)
{
    return JS_DupValue(ctx, func_data[0]);
}

static JSValue js_promise_finally_thrower(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv,
                                          int magic, JSValue *func_data)
{
    return JS_Throw(ctx, JS_DupValue(ctx, func_data[0]));
}

static JSValue js_promise_then_finally_func(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv,
                                            int magic, JSValue *func_data)
{
    JSValueConst ctor = func_data[0];
    JSValueConst onFinally = func_data[1];
    JSValue res, promise, ret, then_func;

    res = JS_Call(ctx, onFinally, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(res))
        return res;
    promise = js_promise_resolve(ctx, ctor, 1, (JSValueConst *)&res, 0);
    JS_FreeValue(ctx, res);
    if (JS_IsException(promise))
        return promise;
    if (magic == 0) {
        then_func = JS_NewCFunctionData(ctx, js_promise_finally_value_thunk, 0,
                                        0, 1, argv);
    } else {
        then_func = JS_NewCFunctionData(ctx, js_promise_finally_thrower, 0,
                                        0, 1, argv);
    }
    if (JS_IsException(then_func)) {
        JS_FreeValue(ctx, promise);
        return then_func;
    }
    ret = JS_InvokeFree(ctx, promise, JS_ATOM_then, 1, (JSValueConst *)&then_func);
    JS_FreeValue(ctx, then_func);
    return ret;
}

static JSValue js_promise_finally(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValueConst onFinally = argv[0];
    JSValue ctor, ret;
    JSValue then_funcs[2];
    JSValueConst func_data[2];
    int i;

    ctor = JS_SpeciesConstructor(ctx, this_val, JS_UNDEFINED);
    if (JS_IsException(ctor))
        return ctor;
    if (!JS_IsFunction(ctx, onFinally)) {
        then_funcs[0] = JS_DupValue(ctx, onFinally);
        then_funcs[1] = JS_DupValue(ctx, onFinally);
    } else {
        func_data[0] = ctor;
        func_data[1] = onFinally;
        for(i = 0; i < 2; i++) {
            then_funcs[i] = JS_NewCFunctionData(ctx, js_promise_then_finally_func, 1, i, 2, func_data);
            if (JS_IsException(then_funcs[i])) {
                if (i == 1)
                    JS_FreeValue(ctx, then_funcs[0]);
                JS_FreeValue(ctx, ctor);
                return JS_EXCEPTION;
            }
        }
    }
    JS_FreeValue(ctx, ctor);
    ret = JS_Invoke(ctx, this_val, JS_ATOM_then, 2, (JSValueConst *)then_funcs);
    JS_FreeValue(ctx, then_funcs[0]);
    JS_FreeValue(ctx, then_funcs[1]);
    return ret;
}

static const JSCFunctionListEntry js_promise_funcs[] = {
    JS_CFUNC_MAGIC_DEF("resolve", 1, js_promise_resolve, 0 ),
    JS_CFUNC_MAGIC_DEF("reject", 1, js_promise_resolve, 1 ),
    JS_CFUNC_MAGIC_DEF("all", 1, js_promise_all, PROMISE_MAGIC_all ),
    JS_CFUNC_MAGIC_DEF("allSettled", 1, js_promise_all, PROMISE_MAGIC_allSettled ),
    JS_CFUNC_MAGIC_DEF("any", 1, js_promise_all, PROMISE_MAGIC_any ),
    JS_CFUNC_DEF("race", 1, js_promise_race ),
    //JS_CFUNC_DEF("__newPromiseCapability", 1, js_promise___newPromiseCapability ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL),
};

static const JSCFunctionListEntry js_promise_proto_funcs[] = {
    JS_CFUNC_DEF("then", 2, js_promise_then ),
    JS_CFUNC_DEF("catch", 1, js_promise_catch ),
    JS_CFUNC_DEF("finally", 1, js_promise_finally ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Promise", JS_PROP_CONFIGURABLE ),
};

/* AsyncFunction */
static const JSCFunctionListEntry js_async_function_proto_funcs[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "AsyncFunction", JS_PROP_CONFIGURABLE ),
};

static JSValue js_async_from_sync_iterator_unwrap(JSContext *ctx,
                                                  JSValueConst this_val,
                                                  int argc, JSValueConst *argv,
                                                  int magic, JSValue *func_data)
{
    return js_create_iterator_result(ctx, JS_DupValue(ctx, argv[0]),
                                     JS_ToBool(ctx, func_data[0]));
}

static JSValue js_async_from_sync_iterator_unwrap_func_create(JSContext *ctx,
                                                              BOOL done)
{
    JSValueConst func_data[1];

    func_data[0] = (JSValueConst)JS_NewBool(ctx, done);
    return JS_NewCFunctionData(ctx, js_async_from_sync_iterator_unwrap,
                               1, 0, 1, func_data);
}

/* AsyncIteratorPrototype */

static const JSCFunctionListEntry js_async_iterator_proto_funcs[] = {
    JS_CFUNC_DEF("[Symbol.asyncIterator]", 0, js_iterator_proto_iterator ),
};

/* AsyncFromSyncIteratorPrototype */

typedef struct JSAsyncFromSyncIteratorData {
    JSValue sync_iter;
    JSValue next_method;
} JSAsyncFromSyncIteratorData;

static void js_async_from_sync_iterator_finalizer(JSRuntime *rt, JSValue val)
{
    JSAsyncFromSyncIteratorData *s =
        JS_GetOpaque(val, JS_CLASS_ASYNC_FROM_SYNC_ITERATOR);
    if (s) {
        JS_FreeValueRT(rt, s->sync_iter);
        JS_FreeValueRT(rt, s->next_method);
        js_free_rt(rt, s);
    }
}

static void js_async_from_sync_iterator_mark(JSRuntime *rt, JSValueConst val,
                                             JS_MarkFunc *mark_func)
{
    JSAsyncFromSyncIteratorData *s =
        JS_GetOpaque(val, JS_CLASS_ASYNC_FROM_SYNC_ITERATOR);
    if (s) {
        JS_MarkValue(rt, s->sync_iter, mark_func);
        JS_MarkValue(rt, s->next_method, mark_func);
    }
}

static JSValue JS_CreateAsyncFromSyncIterator(JSContext *ctx,
                                              JSValueConst sync_iter)
{
    JSValue async_iter, next_method;
    JSAsyncFromSyncIteratorData *s;

    next_method = JS_GetProperty(ctx, sync_iter, JS_ATOM_next);
    if (JS_IsException(next_method))
        return JS_EXCEPTION;
    async_iter = JS_NewObjectClass(ctx, JS_CLASS_ASYNC_FROM_SYNC_ITERATOR);
    if (JS_IsException(async_iter)) {
        JS_FreeValue(ctx, next_method);
        return async_iter;
    }
    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        JS_FreeValue(ctx, async_iter);
        JS_FreeValue(ctx, next_method);
        return JS_EXCEPTION;
    }
    s->sync_iter = JS_DupValue(ctx, sync_iter);
    s->next_method = next_method;
    JS_SetOpaque(async_iter, s);
    return async_iter;
}

static JSValue js_async_from_sync_iterator_next(JSContext *ctx, JSValueConst this_val,
                                                int argc, JSValueConst *argv,
                                                int magic)
{
    JSValue promise, resolving_funcs[2], value, err, method;
    JSAsyncFromSyncIteratorData *s;
    int done;
    int is_reject;

    promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise))
        return JS_EXCEPTION;
    s = JS_GetOpaque(this_val, JS_CLASS_ASYNC_FROM_SYNC_ITERATOR);
    if (!s) {
        JS_ThrowTypeError(ctx, "not an Async-from-Sync Iterator");
        goto reject;
    }

    if (magic == GEN_MAGIC_NEXT) {
        method = JS_DupValue(ctx, s->next_method);
    } else {
        method = JS_GetProperty(ctx, s->sync_iter,
                                magic == GEN_MAGIC_RETURN ? JS_ATOM_return :
                                JS_ATOM_throw);
        if (JS_IsException(method))
            goto reject;
        if (JS_IsUndefined(method) || JS_IsNull(method)) {
            if (magic == GEN_MAGIC_RETURN) {
                err = js_create_iterator_result(ctx, JS_DupValue(ctx, argv[0]), TRUE);
                is_reject = 0;
            } else {
                err = JS_DupValue(ctx, argv[0]);
                is_reject = 1;
            }
            goto done_resolve;
        }
    }
    value = JS_IteratorNext2(ctx, s->sync_iter, method,
                             argc >= 1 ? 1 : 0, argv, &done);
    JS_FreeValue(ctx, method);
    if (JS_IsException(value))
        goto reject;
    if (done == 2) {
        JSValue obj = value;
        value = JS_IteratorGetCompleteValue(ctx, obj, &done);
        JS_FreeValue(ctx, obj);
        if (JS_IsException(value))
            goto reject;
    }

    if (JS_IsException(value)) {
        JSValue res2;
    reject:
        err = JS_GetException(ctx);
        is_reject = 1;
    done_resolve:
        res2 = JS_Call(ctx, resolving_funcs[is_reject], JS_UNDEFINED,
                       1, (JSValueConst *)&err);
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, res2);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        return promise;
    }
    {
        JSValue value_wrapper_promise, resolve_reject[2];
        int res;

        value_wrapper_promise = js_promise_resolve(ctx, ctx->promise_ctor,
                                                   1, (JSValueConst *)&value, 0);
        if (JS_IsException(value_wrapper_promise)) {
            JS_FreeValue(ctx, value);
            goto reject;
        }

        resolve_reject[0] =
            js_async_from_sync_iterator_unwrap_func_create(ctx, done);
        if (JS_IsException(resolve_reject[0])) {
            JS_FreeValue(ctx, value_wrapper_promise);
            goto fail;
        }
        JS_FreeValue(ctx, value);
        resolve_reject[1] = JS_UNDEFINED;

        res = perform_promise_then(ctx, value_wrapper_promise,
                                   (JSValueConst *)resolve_reject,
                                   (JSValueConst *)resolving_funcs);
        JS_FreeValue(ctx, resolve_reject[0]);
        JS_FreeValue(ctx, value_wrapper_promise);
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        if (res) {
            JS_FreeValue(ctx, promise);
            return JS_EXCEPTION;
        }
    }
    return promise;
 fail:
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, resolving_funcs[0]);
    JS_FreeValue(ctx, resolving_funcs[1]);
    JS_FreeValue(ctx, promise);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_async_from_sync_iterator_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("next", 1, js_async_from_sync_iterator_next, GEN_MAGIC_NEXT ),
    JS_CFUNC_MAGIC_DEF("return", 1, js_async_from_sync_iterator_next, GEN_MAGIC_RETURN ),
    JS_CFUNC_MAGIC_DEF("throw", 1, js_async_from_sync_iterator_next, GEN_MAGIC_THROW ),
};

/* AsyncGeneratorFunction */

static const JSCFunctionListEntry js_async_generator_function_proto_funcs[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "AsyncGeneratorFunction", JS_PROP_CONFIGURABLE ),
};

/* AsyncGenerator prototype */

static const JSCFunctionListEntry js_async_generator_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("next", 1, js_async_generator_next, GEN_MAGIC_NEXT ),
    JS_CFUNC_MAGIC_DEF("return", 1, js_async_generator_next, GEN_MAGIC_RETURN ),
    JS_CFUNC_MAGIC_DEF("throw", 1, js_async_generator_next, GEN_MAGIC_THROW ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "AsyncGenerator", JS_PROP_CONFIGURABLE ),
};

static JSClassShortDef const js_async_class_def[] = {
    { JS_ATOM_Promise, js_promise_finalizer, js_promise_mark },                      /* JS_CLASS_PROMISE */
    { JS_ATOM_PromiseResolveFunction, js_promise_resolve_function_finalizer, js_promise_resolve_function_mark }, /* JS_CLASS_PROMISE_RESOLVE_FUNCTION */
    { JS_ATOM_PromiseRejectFunction, js_promise_resolve_function_finalizer, js_promise_resolve_function_mark }, /* JS_CLASS_PROMISE_REJECT_FUNCTION */
    { JS_ATOM_AsyncFunction, js_bytecode_function_finalizer, js_bytecode_function_mark },  /* JS_CLASS_ASYNC_FUNCTION */
    { JS_ATOM_AsyncFunctionResolve, js_async_function_resolve_finalizer, js_async_function_resolve_mark }, /* JS_CLASS_ASYNC_FUNCTION_RESOLVE */
    { JS_ATOM_AsyncFunctionReject, js_async_function_resolve_finalizer, js_async_function_resolve_mark }, /* JS_CLASS_ASYNC_FUNCTION_REJECT */
    { JS_ATOM_empty_string, js_async_from_sync_iterator_finalizer, js_async_from_sync_iterator_mark }, /* JS_CLASS_ASYNC_FROM_SYNC_ITERATOR */
    { JS_ATOM_AsyncGeneratorFunction, js_bytecode_function_finalizer, js_bytecode_function_mark },  /* JS_CLASS_ASYNC_GENERATOR_FUNCTION */
    { JS_ATOM_AsyncGenerator, js_async_generator_finalizer, js_async_generator_mark },  /* JS_CLASS_ASYNC_GENERATOR */
};

void JS_AddIntrinsicPromise(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValue obj1;

    if (!JS_IsRegisteredClass(rt, JS_CLASS_PROMISE)) {
        init_class_range(rt, js_async_class_def, JS_CLASS_PROMISE,
                         countof(js_async_class_def));
        rt->class_array[JS_CLASS_PROMISE_RESOLVE_FUNCTION].call = js_promise_resolve_function_call;
        rt->class_array[JS_CLASS_PROMISE_REJECT_FUNCTION].call = js_promise_resolve_function_call;
        rt->class_array[JS_CLASS_ASYNC_FUNCTION].call = js_async_function_call;
        rt->class_array[JS_CLASS_ASYNC_FUNCTION_RESOLVE].call = js_async_function_resolve_call;
        rt->class_array[JS_CLASS_ASYNC_FUNCTION_REJECT].call = js_async_function_resolve_call;
        rt->class_array[JS_CLASS_ASYNC_GENERATOR_FUNCTION].call = js_async_generator_function_call;
    }

    /* Promise */
    ctx->class_proto[JS_CLASS_PROMISE] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_PROMISE],
                               js_promise_proto_funcs,
                               countof(js_promise_proto_funcs));
    obj1 = JS_NewCFunction2(ctx, js_promise_constructor, "Promise", 1,
                            JS_CFUNC_constructor, 0);
    ctx->promise_ctor = JS_DupValue(ctx, obj1);
    JS_SetPropertyFunctionList(ctx, obj1,
                               js_promise_funcs,
                               countof(js_promise_funcs));
    JS_NewGlobalCConstructor2(ctx, obj1, "Promise",
                              ctx->class_proto[JS_CLASS_PROMISE]);

    /* AsyncFunction */
    ctx->class_proto[JS_CLASS_ASYNC_FUNCTION] = JS_NewObjectProto(ctx, ctx->function_proto);
    obj1 = JS_NewCFunction3(ctx, (JSCFunction *)js_function_constructor,
                            "AsyncFunction", 1,
                            JS_CFUNC_constructor_or_func_magic, JS_FUNC_ASYNC,
                            ctx->function_ctor);
    JS_SetPropertyFunctionList(ctx,
                               ctx->class_proto[JS_CLASS_ASYNC_FUNCTION],
                               js_async_function_proto_funcs,
                               countof(js_async_function_proto_funcs));
    JS_SetConstructor2(ctx, obj1, ctx->class_proto[JS_CLASS_ASYNC_FUNCTION],
                       0, JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, obj1);

    /* AsyncIteratorPrototype */
    ctx->async_iterator_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->async_iterator_proto,
                               js_async_iterator_proto_funcs,
                               countof(js_async_iterator_proto_funcs));

    /* AsyncFromSyncIteratorPrototype */
    ctx->class_proto[JS_CLASS_ASYNC_FROM_SYNC_ITERATOR] =
        JS_NewObjectProto(ctx, ctx->async_iterator_proto);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_ASYNC_FROM_SYNC_ITERATOR],
                               js_async_from_sync_iterator_proto_funcs,
                               countof(js_async_from_sync_iterator_proto_funcs));

    /* AsyncGeneratorPrototype */
    ctx->class_proto[JS_CLASS_ASYNC_GENERATOR] =
        JS_NewObjectProto(ctx, ctx->async_iterator_proto);
    JS_SetPropertyFunctionList(ctx,
                               ctx->class_proto[JS_CLASS_ASYNC_GENERATOR],
                               js_async_generator_proto_funcs,
                               countof(js_async_generator_proto_funcs));

    /* AsyncGeneratorFunction */
    ctx->class_proto[JS_CLASS_ASYNC_GENERATOR_FUNCTION] =
        JS_NewObjectProto(ctx, ctx->function_proto);
    obj1 = JS_NewCFunction3(ctx, (JSCFunction *)js_function_constructor,
                            "AsyncGeneratorFunction", 1,
                            JS_CFUNC_constructor_or_func_magic,
                            JS_FUNC_ASYNC_GENERATOR,
                            ctx->function_ctor);
    JS_SetPropertyFunctionList(ctx,
                               ctx->class_proto[JS_CLASS_ASYNC_GENERATOR_FUNCTION],
                               js_async_generator_function_proto_funcs,
                               countof(js_async_generator_function_proto_funcs));
    JS_SetConstructor2(ctx, ctx->class_proto[JS_CLASS_ASYNC_GENERATOR_FUNCTION],
                       ctx->class_proto[JS_CLASS_ASYNC_GENERATOR],
                       JS_PROP_CONFIGURABLE, JS_PROP_CONFIGURABLE);
    JS_SetConstructor2(ctx, obj1, ctx->class_proto[JS_CLASS_ASYNC_GENERATOR_FUNCTION],
                       0, JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, obj1);
}

/* URI handling */

static int string_get_hex(JSString *p, int k, int n) {
    int c = 0, h;
    while (n-- > 0) {
        if ((h = from_hex(string_get(p, k++))) < 0)
            return -1;
        c = (c << 4) | h;
    }
    return c;
}

static int isURIReserved(int c) {
    return c < 0x100 && memchr(";/?:@&=+$,#", c, sizeof(";/?:@&=+$,#") - 1) != NULL;
}

static int __attribute__((format(printf, 2, 3))) js_throw_URIError(JSContext *ctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    JS_ThrowError(ctx, JS_URI_ERROR, fmt, ap);
    va_end(ap);
    return -1;
}

static int hex_decode(JSContext *ctx, JSString *p, int k) {
    int c;

    if (k >= p->len || string_get(p, k) != '%')
        return js_throw_URIError(ctx, "expecting %%");
    if (k + 2 >= p->len || (c = string_get_hex(p, k + 1, 2)) < 0)
        return js_throw_URIError(ctx, "expecting hex digit");

    return c;
}

static JSValue js_global_decodeURI(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int isComponent)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int k, c, c1, n, c_min;

    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    string_buffer_init(ctx, b, 0);

    p = JS_VALUE_GET_STRING(str);
    for (k = 0; k < p->len;) {
        c = string_get(p, k);
        if (c == '%') {
            c = hex_decode(ctx, p, k);
            if (c < 0)
                goto fail;
            k += 3;
            if (c < 0x80) {
                if (!isComponent && isURIReserved(c)) {
                    c = '%';
                    k -= 2;
                }
            } else {
                /* Decode URI-encoded UTF-8 sequence */
                if (c >= 0xc0 && c <= 0xdf) {
                    n = 1;
                    c_min = 0x80;
                    c &= 0x1f;
                } else if (c >= 0xe0 && c <= 0xef) {
                    n = 2;
                    c_min = 0x800;
                    c &= 0xf;
                } else if (c >= 0xf0 && c <= 0xf7) {
                    n = 3;
                    c_min = 0x10000;
                    c &= 0x7;
                } else {
                    n = 0;
                    c_min = 1;
                    c = 0;
                }
                while (n-- > 0) {
                    c1 = hex_decode(ctx, p, k);
                    if (c1 < 0)
                        goto fail;
                    k += 3;
                    if ((c1 & 0xc0) != 0x80) {
                        c = 0;
                        break;
                    }
                    c = (c << 6) | (c1 & 0x3f);
                }
                if (c < c_min || c > 0x10FFFF ||
                    (c >= 0xd800 && c < 0xe000)) {
                    js_throw_URIError(ctx, "malformed UTF-8");
                    goto fail;
                }
            }
        } else {
            k++;
        }
        string_buffer_putc(b, c);
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    JS_FreeValue(ctx, str);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static int isUnescaped(int c) {
    static char const unescaped_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "@*_+-./";
    return c < 0x100 &&
        memchr(unescaped_chars, c, sizeof(unescaped_chars) - 1);
}

static int isURIUnescaped(int c, int isComponent) {
    return c < 0x100 &&
        ((c >= 0x61 && c <= 0x7a) ||
         (c >= 0x41 && c <= 0x5a) ||
         (c >= 0x30 && c <= 0x39) ||
         memchr("-_.!~*'()", c, sizeof("-_.!~*'()") - 1) != NULL ||
         (!isComponent && isURIReserved(c)));
}

static int encodeURI_hex(StringBuffer *b, int c) {
    uint8_t buf[6];
    int n = 0;
    const char *hex = "0123456789ABCDEF";

    buf[n++] = '%';
    if (c >= 256) {
        buf[n++] = 'u';
        buf[n++] = hex[(c >> 12) & 15];
        buf[n++] = hex[(c >>  8) & 15];
    }
    buf[n++] = hex[(c >> 4) & 15];
    buf[n++] = hex[(c >> 0) & 15];
    return string_buffer_write8(b, buf, n);
}

static JSValue js_global_encodeURI(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv,
                                   int isComponent)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int k, c, c1;

    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    p = JS_VALUE_GET_STRING(str);
    string_buffer_init(ctx, b, p->len);
    for (k = 0; k < p->len;) {
        c = string_get(p, k);
        k++;
        if (isURIUnescaped(c, isComponent)) {
            string_buffer_putc16(b, c);
        } else {
            if (c >= 0xdc00 && c <= 0xdfff) {
                js_throw_URIError(ctx, "invalid character");
                goto fail;
            } else if (c >= 0xd800 && c <= 0xdbff) {
                if (k >= p->len) {
                    js_throw_URIError(ctx, "expecting surrogate pair");
                    goto fail;
                }
                c1 = string_get(p, k);
                k++;
                if (c1 < 0xdc00 || c1 > 0xdfff) {
                    js_throw_URIError(ctx, "expecting surrogate pair");
                    goto fail;
                }
                c = (((c & 0x3ff) << 10) | (c1 & 0x3ff)) + 0x10000;
            }
            if (c < 0x80) {
                encodeURI_hex(b, c);
            } else {
                /* XXX: use C UTF-8 conversion ? */
                if (c < 0x800) {
                    encodeURI_hex(b, (c >> 6) | 0xc0);
                } else {
                    if (c < 0x10000) {
                        encodeURI_hex(b, (c >> 12) | 0xe0);
                    } else {
                        encodeURI_hex(b, (c >> 18) | 0xf0);
                        encodeURI_hex(b, ((c >> 12) & 0x3f) | 0x80);
                    }
                    encodeURI_hex(b, ((c >> 6) & 0x3f) | 0x80);
                }
                encodeURI_hex(b, (c & 0x3f) | 0x80);
            }
        }
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);

fail:
    JS_FreeValue(ctx, str);
    string_buffer_free(b);
    return JS_EXCEPTION;
}

static JSValue js_global_escape(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int i, len, c;

    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    p = JS_VALUE_GET_STRING(str);
    string_buffer_init(ctx, b, p->len);
    for (i = 0, len = p->len; i < len; i++) {
        c = string_get(p, i);
        if (isUnescaped(c)) {
            string_buffer_putc16(b, c);
        } else {
            encodeURI_hex(b, c);
        }
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);
}

static JSValue js_global_unescape(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue str;
    StringBuffer b_s, *b = &b_s;
    JSString *p;
    int i, len, c, n;

    str = JS_ToString(ctx, argv[0]);
    if (JS_IsException(str))
        return str;

    string_buffer_init(ctx, b, 0);
    p = JS_VALUE_GET_STRING(str);
    for (i = 0, len = p->len; i < len; i++) {
        c = string_get(p, i);
        if (c == '%') {
            if (i + 6 <= len
            &&  string_get(p, i + 1) == 'u'
            &&  (n = string_get_hex(p, i + 2, 4)) >= 0) {
                c = n;
                i += 6 - 1;
            } else
            if (i + 3 <= len
            &&  (n = string_get_hex(p, i + 1, 2)) >= 0) {
                c = n;
                i += 3 - 1;
            }
        }
        string_buffer_putc16(b, c);
    }
    JS_FreeValue(ctx, str);
    return string_buffer_end(b);
}

/* global object */

static const JSCFunctionListEntry js_global_funcs[] = {
    JS_CFUNC_DEF("parseInt", 2, js_parseInt ),
    JS_CFUNC_DEF("parseFloat", 1, js_parseFloat ),
    JS_CFUNC_DEF("isNaN", 1, js_global_isNaN ),
    JS_CFUNC_DEF("isFinite", 1, js_global_isFinite ),

    JS_CFUNC_MAGIC_DEF("decodeURI", 1, js_global_decodeURI, 0 ),
    JS_CFUNC_MAGIC_DEF("decodeURIComponent", 1, js_global_decodeURI, 1 ),
    JS_CFUNC_MAGIC_DEF("encodeURI", 1, js_global_encodeURI, 0 ),
    JS_CFUNC_MAGIC_DEF("encodeURIComponent", 1, js_global_encodeURI, 1 ),
    JS_CFUNC_DEF("escape", 1, js_global_escape ),
    JS_CFUNC_DEF("unescape", 1, js_global_unescape ),
    JS_PROP_DOUBLE_DEF("Infinity", 1.0 / 0.0, 0 ),
    JS_PROP_DOUBLE_DEF("NaN", NAN, 0 ),
    JS_PROP_UNDEFINED_DEF("undefined", 0 ),

    /* for the 'Date' implementation */
    JS_CFUNC_DEF("__date_clock", 0, js___date_clock ),
    //JS_CFUNC_DEF("__date_now", 0, js___date_now ),
    //JS_CFUNC_DEF("__date_getTimezoneOffset", 1, js___date_getTimezoneOffset ),
    //JS_CFUNC_DEF("__date_create", 3, js___date_create ),
};

/* Date */

static int64_t math_mod(int64_t a, int64_t b) {
    /* return positive modulo */
    int64_t m = a % b;
    return m + (m < 0) * b;
}

static int64_t floor_div(int64_t a, int64_t b) {
    /* integer division rounding toward -Infinity */
    int64_t m = a % b;
    return (a - (m + (m < 0) * b)) / b;
}

static JSValue js_Date_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);

static __exception int JS_ThisTimeValue(JSContext *ctx, double *valp, JSValueConst this_val)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_DATE && JS_IsNumber(p->u.object_data))
            return JS_ToFloat64(ctx, valp, p->u.object_data);
    }
    JS_ThrowTypeError(ctx, "not a Date object");
    return -1;
}

static JSValue JS_SetThisTimeValue(JSContext *ctx, JSValueConst this_val, double v)
{
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_DATE) {
            JS_FreeValue(ctx, p->u.object_data);
            p->u.object_data = JS_NewFloat64(ctx, v);
            return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a Date object");
}

static int64_t days_from_year(int64_t y) {
    return 365 * (y - 1970) + floor_div(y - 1969, 4) -
        floor_div(y - 1901, 100) + floor_div(y - 1601, 400);
}

static int64_t days_in_year(int64_t y) {
    return 365 + !(y % 4) - !(y % 100) + !(y % 400);
}

/* return the year, update days */
static int64_t year_from_days(int64_t *days) {
    int64_t y, d1, nd, d = *days;
    y = floor_div(d * 10000, 3652425) + 1970;
    /* the initial approximation is very good, so only a few
       iterations are necessary */
    for(;;) {
        d1 = d - days_from_year(y);
        if (d1 < 0) {
            y--;
            d1 += days_in_year(y);
        } else {
            nd = days_in_year(y);
            if (d1 < nd)
                break;
            d1 -= nd;
            y++;
        }
    }
    *days = d1;
    return y;
}

static int const month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static char const month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
static char const day_names[] = "SunMonTueWedThuFriSat";

static __exception int get_date_fields(JSContext *ctx, JSValueConst obj,
                                       double fields[9], int is_local, int force)
{
    double dval;
    int64_t d, days, wd, y, i, md, h, m, s, ms, tz = 0;

    if (JS_ThisTimeValue(ctx, &dval, obj))
        return -1;

    if (isnan(dval)) {
        if (!force)
            return FALSE; /* NaN */
        d = 0;        /* initialize all fields to 0 */
    } else {
        d = dval;
        if (is_local) {
            tz = -getTimezoneOffset(d);
            d += tz * 60000;
        }
    }

    /* result is >= 0, we can use % */
    h = math_mod(d, 86400000);
    days = (d - h) / 86400000;
    ms = h % 1000;
    h = (h - ms) / 1000;
    s = h % 60;
    h = (h - s) / 60;
    m = h % 60;
    h = (h - m) / 60;
    wd = math_mod(days + 4, 7); /* week day */
    y = year_from_days(&days);

    for(i = 0; i < 11; i++) {
        md = month_days[i];
        if (i == 1)
            md += days_in_year(y) - 365;
        if (days < md)
            break;
        days -= md;
    }
    fields[0] = y;
    fields[1] = i;
    fields[2] = days + 1;
    fields[3] = h;
    fields[4] = m;
    fields[5] = s;
    fields[6] = ms;
    fields[7] = wd;
    fields[8] = tz;
    return TRUE;
}

static double time_clip(double t) {
    if (t >= -8.64e15 && t <= 8.64e15)
        return trunc(t) + 0.0;  /* convert -0 to +0 */
    else
        return NAN;
}

/* The spec mandates the use of 'double' and it fixes the order
   of the operations */
static double set_date_fields(double fields[], int is_local) {
    int64_t y;
    double days, d, h, m1;
    int i, m, md;
    
    m1 = fields[1];
    m = fmod(m1, 12);
    if (m < 0)
        m += 12;
    y = (int64_t)(fields[0] + floor(m1 / 12));
    days = days_from_year(y);

    for(i = 0; i < m; i++) {
        md = month_days[i];
        if (i == 1)
            md += days_in_year(y) - 365;
        days += md;
    }
    days += fields[2] - 1;
    h = fields[3] * 3600000 + fields[4] * 60000 + 
        fields[5] * 1000 + fields[6];
    d = days * 86400000 + h;
    if (is_local)
        d += getTimezoneOffset(d) * 60000;
    return time_clip(d);
}

static JSValue get_date_field(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    // get_date_field(obj, n, is_local)
    double fields[9];
    int res, n, is_local;

    is_local = magic & 0x0F;
    n = (magic >> 4) & 0x0F;
    res = get_date_fields(ctx, this_val, fields, is_local, 0);
    if (res < 0)
        return JS_EXCEPTION;
    if (!res)
        return JS_NAN;

    if (magic & 0x100) {    // getYear
        fields[0] -= 1900;
    }
    return JS_NewFloat64(ctx, fields[n]);
}

static JSValue set_date_field(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    // _field(obj, first_field, end_field, args, is_local)
    double fields[9];
    int res, first_field, end_field, is_local, i, n;
    double d, a;

    d = NAN;
    first_field = (magic >> 8) & 0x0F;
    end_field = (magic >> 4) & 0x0F;
    is_local = magic & 0x0F;

    res = get_date_fields(ctx, this_val, fields, is_local, first_field == 0);
    if (res < 0)
        return JS_EXCEPTION;
    if (res && argc > 0) {
        n = end_field - first_field;
        if (argc < n)
            n = argc;
        for(i = 0; i < n; i++) {
            if (JS_ToFloat64(ctx, &a, argv[i]))
                return JS_EXCEPTION;
            if (!isfinite(a))
                goto done;
            fields[first_field + i] = trunc(a);
        }
        d = set_date_fields(fields, is_local);
    }
done:
    return JS_SetThisTimeValue(ctx, this_val, d);
}

/* fmt:
   0: toUTCString: "Tue, 02 Jan 2018 23:04:46 GMT"
   1: toString: "Wed Jan 03 2018 00:05:22 GMT+0100 (CET)"
   2: toISOString: "2018-01-02T23:02:56.927Z"
   3: toLocaleString: "1/2/2018, 11:40:40 PM"
   part: 1=date, 2=time 3=all
   XXX: should use a variant of strftime().
 */
static JSValue get_date_string(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    // _string(obj, fmt, part)
    char buf[64];
    double fields[9];
    int res, fmt, part, pos;
    int y, mon, d, h, m, s, ms, wd, tz;

    fmt = (magic >> 4) & 0x0F;
    part = magic & 0x0F;

    res = get_date_fields(ctx, this_val, fields, fmt & 1, 0);
    if (res < 0)
        return JS_EXCEPTION;
    if (!res) {
        if (fmt == 2)
            return JS_ThrowRangeError(ctx, "Date value is NaN");
        else
            return JS_NewString(ctx, "Invalid Date");
    }

    y = fields[0];
    mon = fields[1];
    d = fields[2];
    h = fields[3];
    m = fields[4];
    s = fields[5];
    ms = fields[6];
    wd = fields[7];
    tz = fields[8];

    pos = 0;

    if (part & 1) { /* date part */
        switch(fmt) {
        case 0:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%.3s, %02d %.3s %0*d ",
                            day_names + wd * 3, d,
                            month_names + mon * 3, 4 + (y < 0), y);
            break;
        case 1:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%.3s %.3s %02d %0*d",
                            day_names + wd * 3,
                            month_names + mon * 3, d, 4 + (y < 0), y);
            if (part == 3) {
                buf[pos++] = ' ';
            }
            break;
        case 2:
            if (y >= 0 && y <= 9999) {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "%04d", y);
            } else {
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "%+07d", y);
            }
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "-%02d-%02dT", mon + 1, d);
            break;
        case 3:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d/%02d/%0*d", mon + 1, d, 4 + (y < 0), y);
            if (part == 3) {
                buf[pos++] = ',';
                buf[pos++] = ' ';
            }
            break;
        }
    }
    if (part & 2) { /* time part */
        switch(fmt) {
        case 0:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d GMT", h, m, s);
            break;
        case 1:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d GMT", h, m, s);
            if (tz < 0) {
                buf[pos++] = '-';
                tz = -tz;
            } else {
                buf[pos++] = '+';
            }
            /* tz is >= 0, can use % */
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d%02d", tz / 60, tz % 60);
            /* XXX: tack the time zone code? */
            break;
        case 2:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d.%03dZ", h, m, s, ms);
            break;
        case 3:
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%02d:%02d:%02d %cM", (h + 1) % 12 - 1, m, s,
                            (h < 12) ? 'A' : 'P');
            break;
        }
    }
    return JS_NewStringLen(ctx, buf, pos);
}

/* OS dependent: return the UTC time in ms since 1970. */
static int64_t date_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

static JSValue js_date_constructor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    // Date(y, mon, d, h, m, s, ms)
    JSValue rv;
    int i, n;
    double a, val;

    if (JS_IsUndefined(new_target)) {
        /* invoked as function */
        argc = 0;
    }
    n = argc;
    if (n == 0) {
        val = date_now();
    } else if (n == 1) {
        JSValue v, dv;
        if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
            JSObject *p = JS_VALUE_GET_OBJ(argv[0]);
            if (p->class_id == JS_CLASS_DATE && JS_IsNumber(p->u.object_data)) {
                if (JS_ToFloat64(ctx, &val, p->u.object_data))
                    return JS_EXCEPTION;
                val = time_clip(val);
                goto has_val;
            }
        }
        v = JS_ToPrimitive(ctx, argv[0], HINT_NONE);
        if (JS_IsString(v)) {
            dv = js_Date_parse(ctx, JS_UNDEFINED, 1, (JSValueConst *)&v);
            JS_FreeValue(ctx, v);
            if (JS_IsException(dv))
                return JS_EXCEPTION;
            if (JS_ToFloat64Free(ctx, &val, dv))
                return JS_EXCEPTION;
        } else {
            if (JS_ToFloat64Free(ctx, &val, v))
                return JS_EXCEPTION;
        }
        val = time_clip(val);
    } else {
        double fields[] = { 0, 0, 1, 0, 0, 0, 0 };
        if (n > 7)
            n = 7;
        for(i = 0; i < n; i++) {
            if (JS_ToFloat64(ctx, &a, argv[i]))
                return JS_EXCEPTION;
            if (!isfinite(a))
                break;
            fields[i] = trunc(a);
            if (i == 0 && fields[0] >= 0 && fields[0] < 100)
                fields[0] += 1900;
        }
        val = (i == n) ? set_date_fields(fields, 1) : NAN;
    }
has_val:
#if 0
    JSValueConst args[3];
    args[0] = new_target;
    args[1] = ctx->class_proto[JS_CLASS_DATE];
    args[2] = JS_NewFloat64(ctx, val);
    rv = js___date_create(ctx, JS_UNDEFINED, 3, args);
#else
    rv = js_create_from_ctor(ctx, new_target, JS_CLASS_DATE);
    if (!JS_IsException(rv))
        JS_SetObjectData(ctx, rv, JS_NewFloat64(ctx, val));
#endif
    if (!JS_IsException(rv) && JS_IsUndefined(new_target)) {
        /* invoked as a function, return (new Date()).toString(); */
        JSValue s;
        s = get_date_string(ctx, rv, 0, NULL, 0x13);
        JS_FreeValue(ctx, rv);
        rv = s;
    }
    return rv;
}

static JSValue js_Date_UTC(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    // UTC(y, mon, d, h, m, s, ms)
    double fields[] = { 0, 0, 1, 0, 0, 0, 0 };
    int i, n;
    double a;

    n = argc;
    if (n == 0)
        return JS_NAN;
    if (n > 7)
        n = 7;
    for(i = 0; i < n; i++) {
        if (JS_ToFloat64(ctx, &a, argv[i]))
            return JS_EXCEPTION;
        if (!isfinite(a))
            return JS_NAN;
        fields[i] = trunc(a);
        if (i == 0 && fields[0] >= 0 && fields[0] < 100)
            fields[0] += 1900;
    }
    return JS_NewFloat64(ctx, set_date_fields(fields, 0));
}

static void string_skip_spaces(JSString *sp, int *pp) {
    while (*pp < sp->len && string_get(sp, *pp) == ' ')
        *pp += 1;
}

static void string_skip_non_spaces(JSString *sp, int *pp) {
    while (*pp < sp->len && string_get(sp, *pp) != ' ')
        *pp += 1;
}

/* parse a numeric field with an optional sign if accept_sign is TRUE */
static int string_get_digits(JSString *sp, int *pp, int64_t *pval) {
    int64_t v = 0;
    int c, p = *pp, p_start;
    
    if (p >= sp->len)
        return -1;
    p_start = p;
    while (p < sp->len) {
        c = string_get(sp, p);
        if (!(c >= '0' && c <= '9')) {
            if (p == p_start)
                return -1;
            else
                break;
        }
        v = v * 10 + c - '0';
        p++;
    }
    *pval = v;
    *pp = p;
    return 0;
}

static int string_get_signed_digits(JSString *sp, int *pp, int64_t *pval) {
    int res, sgn, p = *pp;
    
    if (p >= sp->len)
        return -1;

    sgn = string_get(sp, p);
    if (sgn == '-' || sgn == '+')
        p++;
 
    res = string_get_digits(sp, &p, pval);
    if (res == 0 && sgn == '-')
        *pval = -*pval;
    *pp = p;
    return res;
}

/* parse a fixed width numeric field */
static int string_get_fixed_width_digits(JSString *sp, int *pp, int n, int64_t *pval) {
    int64_t v = 0;
    int i, c, p = *pp;

    for(i = 0; i < n; i++) {
        if (p >= sp->len)
            return -1;
        c = string_get(sp, p);
        if (!(c >= '0' && c <= '9'))
            return -1;
        v = v * 10 + c - '0';
        p++;
    }
    *pval = v;
    *pp = p;
    return 0;
}

static int string_get_milliseconds(JSString *sp, int *pp, int64_t *pval) {
    /* parse milliseconds as a fractional part, round to nearest */
    /* XXX: the spec does not indicate which rounding should be used */
    int mul = 1000, ms = 0, p = *pp, c, p_start;
    if (p >= sp->len)
        return -1;
    p_start = p;
    while (p < sp->len) {
        c = string_get(sp, p);
        if (!(c >= '0' && c <= '9')) {
            if (p == p_start)
                return -1;
            else
                break;
        }
        if (mul == 1 && c >= '5')
            ms += 1;
        ms += (c - '0') * (mul /= 10);
        p++;
    }
    *pval = ms;
    *pp = p;
    return 0;
}


static int find_abbrev(JSString *sp, int p, const char *list, int count) {
    int n, i;

    if (p + 3 <= sp->len) {
        for (n = 0; n < count; n++) {
            for (i = 0; i < 3; i++) {
                if (string_get(sp, p + i) != month_names[n * 3 + i])
                    goto next;
            }
            return n;
        next:;
        }
    }
    return -1;
}

static int string_get_month(JSString *sp, int *pp, int64_t *pval) {
    int n;

    string_skip_spaces(sp, pp);
    n = find_abbrev(sp, *pp, month_names, 12);
    if (n < 0)
        return -1;

    *pval = n;
    *pp += 3;
    return 0;
}

static JSValue js_Date_parse(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    // parse(s)
    JSValue s, rv;
    int64_t fields[] = { 0, 1, 1, 0, 0, 0, 0 };
    double fields1[7];
    int64_t tz, hh, mm;
    double d;
    int p, i, c, sgn, l;
    JSString *sp;
    BOOL is_local;
    
    rv = JS_NAN;

    s = JS_ToString(ctx, argv[0]);
    if (JS_IsException(s))
        return JS_EXCEPTION;
    
    sp = JS_VALUE_GET_STRING(s);
    p = 0;
    if (p < sp->len && (((c = string_get(sp, p)) >= '0' && c <= '9') || c == '+' || c == '-')) {
        /* ISO format */
        /* year field can be negative */
        if (string_get_signed_digits(sp, &p, &fields[0]))
            goto done;

        for (i = 1; i < 7; i++) {
            if (p >= sp->len)
                break;
            switch(i) {
            case 1:
            case 2:
                c = '-';
                break;
            case 3:
                c = 'T';
                break;
            case 4:
            case 5:
                c = ':';
                break;
            case 6:
                c = '.';
                break;
            }
            if (string_get(sp, p) != c)
                break;
            p++;
            if (i == 6) {
                if (string_get_milliseconds(sp, &p, &fields[i]))
                    goto done;
            } else {
                if (string_get_digits(sp, &p, &fields[i]))
                    goto done;
            }
        }
        /* no time: UTC by default */
        is_local = (i > 3);
        fields[1] -= 1;

        /* parse the time zone offset if present: [+-]HH:mm or [+-]HHmm */
        tz = 0;
        if (p < sp->len) {
            sgn = string_get(sp, p);
            if (sgn == '+' || sgn == '-') {
                p++;
                l = sp->len - p;
                if (l != 4 && l != 5)
                    goto done;
                if (string_get_fixed_width_digits(sp, &p, 2, &hh))
                    goto done;
                if (l == 5) {
                    if (string_get(sp, p) != ':')
                        goto done;
                    p++;
                }
                if (string_get_fixed_width_digits(sp, &p, 2, &mm))
                    goto done;
                tz = hh * 60 + mm;
                if (sgn == '-')
                    tz = -tz;
                is_local = FALSE;
            } else if (sgn == 'Z') {
                p++;
                is_local = FALSE;
            } else {
                goto done;
            }
            /* error if extraneous characters */
            if (p != sp->len)
                goto done;
        }
    } else {
        /* toString or toUTCString format */
        /* skip the day of the week */
        string_skip_non_spaces(sp, &p);
        string_skip_spaces(sp, &p);
        if (p >= sp->len)
            goto done;
        c = string_get(sp, p);
        if (c >= '0' && c <= '9') {
            /* day of month first */
            if (string_get_digits(sp, &p, &fields[2]))
                goto done;
            if (string_get_month(sp, &p, &fields[1]))
                goto done;
        } else {
            /* month first */
            if (string_get_month(sp, &p, &fields[1]))
                goto done;
            string_skip_spaces(sp, &p);
            if (string_get_digits(sp, &p, &fields[2]))
                goto done;
        }
        /* year */
        string_skip_spaces(sp, &p);
        if (string_get_signed_digits(sp, &p, &fields[0]))
            goto done;

        /* hour, min, seconds */
        string_skip_spaces(sp, &p);
        for(i = 0; i < 3; i++) {
            if (i == 1 || i == 2) {
                if (p >= sp->len)
                    goto done;
                if (string_get(sp, p) != ':')
                    goto done;
                p++;
            }
            if (string_get_digits(sp, &p, &fields[3 + i]))
                goto done;
        }
        // XXX: parse optional milliseconds?

        /* parse the time zone offset if present: [+-]HHmm */
        is_local = FALSE;
        tz = 0;
        for (tz = 0; p < sp->len; p++) {
            sgn = string_get(sp, p);
            if (sgn == '+' || sgn == '-') {
                p++;
                if (string_get_fixed_width_digits(sp, &p, 2, &hh))
                    goto done;
                if (string_get_fixed_width_digits(sp, &p, 2, &mm))
                    goto done;
                tz = hh * 60 + mm;
                if (sgn == '-')
                    tz = -tz;
                break;
            }
        }
    }
    for(i = 0; i < 7; i++)
        fields1[i] = fields[i];
    d = set_date_fields(fields1, is_local) - tz * 60000;
    rv = JS_NewFloat64(ctx, d);

done:
    JS_FreeValue(ctx, s);
    return rv;
}

static JSValue js_Date_now(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    // now()
    return JS_NewInt64(ctx, date_now());
}

static JSValue js_date_Symbol_toPrimitive(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    // Symbol_toPrimitive(hint)
    JSValueConst obj = this_val;
    JSAtom hint = JS_ATOM_NULL;
    int hint_num;

    if (!JS_IsObject(obj))
        return JS_ThrowTypeErrorNotAnObject(ctx);

    if (JS_IsString(argv[0])) {
        hint = JS_ValueToAtom(ctx, argv[0]);
        if (hint == JS_ATOM_NULL)
            return JS_EXCEPTION;
        JS_FreeAtom(ctx, hint);
    }
    switch (hint) {
    case JS_ATOM_number:
#ifdef CONFIG_BIGNUM
    case JS_ATOM_integer:
#endif
        hint_num = HINT_NUMBER;
        break;
    case JS_ATOM_string:
    case JS_ATOM_default:
        hint_num = HINT_STRING;
        break;
    default:
        return JS_ThrowTypeError(ctx, "invalid hint");
    }
    return JS_ToPrimitive(ctx, obj, hint_num | HINT_FORCE_ORDINARY);
}

static JSValue js_date_getTimezoneOffset(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    // getTimezoneOffset()
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val))
        return JS_EXCEPTION;
    if (isnan(v))
        return JS_NAN;
    else
        return JS_NewInt64(ctx, getTimezoneOffset((int64_t)trunc(v)));
}

static JSValue js_date_getTime(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // getTime()
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val))
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, v);
}

static JSValue js_date_setTime(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // setTime(v)
    double v;

    if (JS_ThisTimeValue(ctx, &v, this_val) || JS_ToFloat64(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    return JS_SetThisTimeValue(ctx, this_val, time_clip(v));
}

static JSValue js_date_setYear(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    // setYear(y)
    double y;
    JSValueConst args[1];

    if (JS_ThisTimeValue(ctx, &y, this_val) || JS_ToFloat64(ctx, &y, argv[0]))
        return JS_EXCEPTION;
    y = +y;
    if (isfinite(y)) {
        y = trunc(y);
        if (y >= 0 && y < 100)
            y += 1900;
    }
    args[0] = JS_NewFloat64(ctx, y);
    return set_date_field(ctx, this_val, 1, args, 0x011);
}

static JSValue js_date_toJSON(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    // toJSON(key)
    JSValue obj, tv, method, rv;
    double d;

    rv = JS_EXCEPTION;
    tv = JS_UNDEFINED;

    obj = JS_ToObject(ctx, this_val);
    tv = JS_ToPrimitive(ctx, obj, HINT_NUMBER);
    if (JS_IsException(tv))
        goto exception;
    if (JS_IsNumber(tv)) {
        if (JS_ToFloat64(ctx, &d, tv) < 0)
            goto exception;
        if (!isfinite(d)) {
            rv = JS_NULL;
            goto done;
        }
    }
    method = JS_GetPropertyStr(ctx, obj, "toISOString");
    if (JS_IsException(method))
        goto exception;
    if (!JS_IsFunction(ctx, method)) {
        JS_ThrowTypeError(ctx, "object needs toISOString method");
        JS_FreeValue(ctx, method);
        goto exception;
    }
    rv = JS_CallFree(ctx, method, obj, 0, NULL);
exception:
done:
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, tv);
    return rv;
}

static const JSCFunctionListEntry js_date_funcs[] = {
    JS_CFUNC_DEF("now", 0, js_Date_now ),
    JS_CFUNC_DEF("parse", 1, js_Date_parse ),
    JS_CFUNC_DEF("UTC", 7, js_Date_UTC ),
};

static const JSCFunctionListEntry js_date_proto_funcs[] = {
    JS_CFUNC_DEF("valueOf", 0, js_date_getTime ),
    JS_CFUNC_MAGIC_DEF("toString", 0, get_date_string, 0x13 ),
    JS_CFUNC_DEF("[Symbol.toPrimitive]", 1, js_date_Symbol_toPrimitive ),
    JS_CFUNC_MAGIC_DEF("toUTCString", 0, get_date_string, 0x03 ),
    JS_ALIAS_DEF("toGMTString", "toUTCString" ),
    JS_CFUNC_MAGIC_DEF("toISOString", 0, get_date_string, 0x23 ),
    JS_CFUNC_MAGIC_DEF("toDateString", 0, get_date_string, 0x11 ),
    JS_CFUNC_MAGIC_DEF("toTimeString", 0, get_date_string, 0x12 ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, get_date_string, 0x33 ),
    JS_CFUNC_MAGIC_DEF("toLocaleDateString", 0, get_date_string, 0x31 ),
    JS_CFUNC_MAGIC_DEF("toLocaleTimeString", 0, get_date_string, 0x32 ),
    JS_CFUNC_DEF("getTimezoneOffset", 0, js_date_getTimezoneOffset ),
    JS_CFUNC_DEF("getTime", 0, js_date_getTime ),
    JS_CFUNC_MAGIC_DEF("getYear", 0, get_date_field, 0x101 ),
    JS_CFUNC_MAGIC_DEF("getFullYear", 0, get_date_field, 0x01 ),
    JS_CFUNC_MAGIC_DEF("getUTCFullYear", 0, get_date_field, 0x00 ),
    JS_CFUNC_MAGIC_DEF("getMonth", 0, get_date_field, 0x11 ),
    JS_CFUNC_MAGIC_DEF("getUTCMonth", 0, get_date_field, 0x10 ),
    JS_CFUNC_MAGIC_DEF("getDate", 0, get_date_field, 0x21 ),
    JS_CFUNC_MAGIC_DEF("getUTCDate", 0, get_date_field, 0x20 ),
    JS_CFUNC_MAGIC_DEF("getHours", 0, get_date_field, 0x31 ),
    JS_CFUNC_MAGIC_DEF("getUTCHours", 0, get_date_field, 0x30 ),
    JS_CFUNC_MAGIC_DEF("getMinutes", 0, get_date_field, 0x41 ),
    JS_CFUNC_MAGIC_DEF("getUTCMinutes", 0, get_date_field, 0x40 ),
    JS_CFUNC_MAGIC_DEF("getSeconds", 0, get_date_field, 0x51 ),
    JS_CFUNC_MAGIC_DEF("getUTCSeconds", 0, get_date_field, 0x50 ),
    JS_CFUNC_MAGIC_DEF("getMilliseconds", 0, get_date_field, 0x61 ),
    JS_CFUNC_MAGIC_DEF("getUTCMilliseconds", 0, get_date_field, 0x60 ),
    JS_CFUNC_MAGIC_DEF("getDay", 0, get_date_field, 0x71 ),
    JS_CFUNC_MAGIC_DEF("getUTCDay", 0, get_date_field, 0x70 ),
    JS_CFUNC_DEF("setTime", 1, js_date_setTime ),
    JS_CFUNC_MAGIC_DEF("setMilliseconds", 1, set_date_field, 0x671 ),
    JS_CFUNC_MAGIC_DEF("setUTCMilliseconds", 1, set_date_field, 0x670 ),
    JS_CFUNC_MAGIC_DEF("setSeconds", 2, set_date_field, 0x571 ),
    JS_CFUNC_MAGIC_DEF("setUTCSeconds", 2, set_date_field, 0x570 ),
    JS_CFUNC_MAGIC_DEF("setMinutes", 3, set_date_field, 0x471 ),
    JS_CFUNC_MAGIC_DEF("setUTCMinutes", 3, set_date_field, 0x470 ),
    JS_CFUNC_MAGIC_DEF("setHours", 4, set_date_field, 0x371 ),
    JS_CFUNC_MAGIC_DEF("setUTCHours", 4, set_date_field, 0x370 ),
    JS_CFUNC_MAGIC_DEF("setDate", 1, set_date_field, 0x231 ),
    JS_CFUNC_MAGIC_DEF("setUTCDate", 1, set_date_field, 0x230 ),
    JS_CFUNC_MAGIC_DEF("setMonth", 2, set_date_field, 0x131 ),
    JS_CFUNC_MAGIC_DEF("setUTCMonth", 2, set_date_field, 0x130 ),
    JS_CFUNC_DEF("setYear", 1, js_date_setYear ),
    JS_CFUNC_MAGIC_DEF("setFullYear", 3, set_date_field, 0x031 ),
    JS_CFUNC_MAGIC_DEF("setUTCFullYear", 3, set_date_field, 0x030 ),
    JS_CFUNC_DEF("toJSON", 1, js_date_toJSON ),
};

void JS_AddIntrinsicDate(JSContext *ctx)
{
    JSValueConst obj;

    /* Date */
    ctx->class_proto[JS_CLASS_DATE] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_DATE], js_date_proto_funcs,
                               countof(js_date_proto_funcs));
    obj = JS_NewGlobalCConstructor(ctx, "Date", js_date_constructor, 7,
                                   ctx->class_proto[JS_CLASS_DATE]);
    JS_SetPropertyFunctionList(ctx, obj, js_date_funcs, countof(js_date_funcs));
}

/* eval */

void JS_AddIntrinsicEval(JSContext *ctx)
{
    ctx->eval_internal = __JS_EvalInternal;
}

#ifdef CONFIG_BIGNUM

/* Operators */

static void js_operator_set_finalizer(JSRuntime *rt, JSValue val)
{
    JSOperatorSetData *opset = JS_GetOpaque(val, JS_CLASS_OPERATOR_SET);
    int i, j;
    JSBinaryOperatorDefEntry *ent;
    
    if (opset) {
        for(i = 0; i < JS_OVOP_COUNT; i++) {
            if (opset->self_ops[i])
                JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, opset->self_ops[i]));
        }
        for(j = 0; j < opset->left.count; j++) {
            ent = &opset->left.tab[j];
            for(i = 0; i < JS_OVOP_BINARY_COUNT; i++) {
                if (ent->ops[i])
                    JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ent->ops[i]));
            }
        }
        js_free_rt(rt, opset->left.tab);
        for(j = 0; j < opset->right.count; j++) {
            ent = &opset->right.tab[j];
            for(i = 0; i < JS_OVOP_BINARY_COUNT; i++) {
                if (ent->ops[i])
                    JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ent->ops[i]));
            }
        }
        js_free_rt(rt, opset->right.tab);
        js_free_rt(rt, opset);
    }
}

static void js_operator_set_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func)
{
    JSOperatorSetData *opset = JS_GetOpaque(val, JS_CLASS_OPERATOR_SET);
    int i, j;
    JSBinaryOperatorDefEntry *ent;
    
    if (opset) {
        for(i = 0; i < JS_OVOP_COUNT; i++) {
            if (opset->self_ops[i])
                JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, opset->self_ops[i]),
                             mark_func);
        }
        for(j = 0; j < opset->left.count; j++) {
            ent = &opset->left.tab[j];
            for(i = 0; i < JS_OVOP_BINARY_COUNT; i++) {
                if (ent->ops[i])
                    JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, ent->ops[i]),
                                 mark_func);
            }
        }
        for(j = 0; j < opset->right.count; j++) {
            ent = &opset->right.tab[j];
            for(i = 0; i < JS_OVOP_BINARY_COUNT; i++) {
                if (ent->ops[i])
                    JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, ent->ops[i]),
                                 mark_func);
            }
        }
    }
}


/* create an OperatorSet object */
static JSValue js_operators_create_internal(JSContext *ctx,
                                            int argc, JSValueConst *argv,
                                            BOOL is_primitive)
{
    JSValue opset_obj, prop, obj;
    JSOperatorSetData *opset, *opset1;
    JSBinaryOperatorDef *def;
    JSValueConst arg;
    int i, j;
    JSBinaryOperatorDefEntry *new_tab;
    JSBinaryOperatorDefEntry *ent;
    uint32_t op_count;

    if (ctx->rt->operator_count == UINT32_MAX) {
        return JS_ThrowTypeError(ctx, "too many operators");
    }
    opset_obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_OPERATOR_SET);
    if (JS_IsException(opset_obj))
        goto fail;
    opset = js_mallocz(ctx, sizeof(*opset));
    if (!opset)
        goto fail;
    JS_SetOpaque(opset_obj, opset);
    if (argc >= 1) {
        arg = argv[0];
        /* self operators */
        for(i = 0; i < JS_OVOP_COUNT; i++) {
            prop = JS_GetPropertyStr(ctx, arg, js_overloadable_operator_names[i]);
            if (JS_IsException(prop))
                goto fail;
            if (!JS_IsUndefined(prop)) {
                if (check_function(ctx, prop)) {
                    JS_FreeValue(ctx, prop);
                    goto fail;
                }
                opset->self_ops[i] = JS_VALUE_GET_OBJ(prop);
            }
        }
    }
    /* left & right operators */
    for(j = 1; j < argc; j++) {
        arg = argv[j];
        prop = JS_GetPropertyStr(ctx, arg, "left");
        if (JS_IsException(prop))
            goto fail;
        def = &opset->right;
        if (JS_IsUndefined(prop)) {
            prop = JS_GetPropertyStr(ctx, arg, "right");
            if (JS_IsException(prop))
                goto fail;
            if (JS_IsUndefined(prop)) {
                JS_ThrowTypeError(ctx, "left or right property must be present");
                goto fail;
            }
            def = &opset->left;
        }
        /* get the operator set */
        obj = JS_GetProperty(ctx, prop, JS_ATOM_prototype);
        JS_FreeValue(ctx, prop);
        if (JS_IsException(obj))
            goto fail;
        prop = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_operatorSet);
        JS_FreeValue(ctx, obj);
        if (JS_IsException(prop))
            goto fail;
        opset1 = JS_GetOpaque2(ctx, prop, JS_CLASS_OPERATOR_SET);
        if (!opset1) {
            JS_FreeValue(ctx, prop);
            goto fail;
        }
        op_count = opset1->operator_counter;
        JS_FreeValue(ctx, prop);
        
        /* we assume there are few entries */
        new_tab = js_realloc(ctx, def->tab,
                             (def->count + 1) * sizeof(def->tab[0]));
        if (!new_tab)
            goto fail;
        def->tab = new_tab;
        def->count++;
        ent = def->tab + def->count - 1;
        memset(ent, 0, sizeof(def->tab[0]));
        ent->operator_index = op_count;
        
        for(i = 0; i < JS_OVOP_BINARY_COUNT; i++) {
            prop = JS_GetPropertyStr(ctx, arg,
                                     js_overloadable_operator_names[i]);
            if (JS_IsException(prop))
                goto fail;
            if (!JS_IsUndefined(prop)) {
                if (check_function(ctx, prop)) {
                    JS_FreeValue(ctx, prop);
                    goto fail;
                }
                ent->ops[i] = JS_VALUE_GET_OBJ(prop);
            }
        }
    }
    opset->is_primitive = is_primitive;
    opset->operator_counter = ctx->rt->operator_count++;
    return opset_obj;
 fail:
    JS_FreeValue(ctx, opset_obj);
    return JS_EXCEPTION;
}

static JSValue js_operators_create(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return js_operators_create_internal(ctx, argc, argv, FALSE);
}

static JSValue js_operators_updateBigIntOperators(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    JSValue opset_obj, prop;
    JSOperatorSetData *opset;
    const JSOverloadableOperatorEnum ops[2] = { JS_OVOP_DIV, JS_OVOP_POW };
    JSOverloadableOperatorEnum op;
    int i;
    
    opset_obj = JS_GetProperty(ctx, ctx->class_proto[JS_CLASS_BIG_INT],
                               JS_ATOM_Symbol_operatorSet);
    if (JS_IsException(opset_obj))
        goto fail;
    opset = JS_GetOpaque2(ctx, opset_obj, JS_CLASS_OPERATOR_SET);
    if (!opset)
        goto fail;
    for(i = 0; i < countof(ops); i++) {
        op = ops[i];
        prop = JS_GetPropertyStr(ctx, argv[0],
                                 js_overloadable_operator_names[op]);
        if (JS_IsException(prop))
            goto fail;
        if (!JS_IsUndefined(prop)) {
            if (!JS_IsNull(prop) && check_function(ctx, prop)) {
                JS_FreeValue(ctx, prop);
                goto fail;
            }
            if (opset->self_ops[op])
                JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, opset->self_ops[op]));
            if (JS_IsNull(prop)) {
                opset->self_ops[op] = NULL;
            } else {
                opset->self_ops[op] = JS_VALUE_GET_PTR(prop);
            }
        }
    }
    JS_FreeValue(ctx, opset_obj);
    return JS_UNDEFINED;
 fail:
    JS_FreeValue(ctx, opset_obj);
    return JS_EXCEPTION;
}

static int js_operators_set_default(JSContext *ctx, JSValueConst obj)
{
    JSValue opset_obj;

    if (!JS_IsObject(obj)) /* in case the prototype is not defined */
        return 0;
    opset_obj = js_operators_create_internal(ctx, 0, NULL, TRUE);
    if (JS_IsException(opset_obj))
        return -1;
    /* cannot be modified by the user */
    JS_DefinePropertyValue(ctx, obj, JS_ATOM_Symbol_operatorSet,
                           opset_obj, 0);
    return 0;
}

static JSValue js_dummy_operators_ctor(JSContext *ctx, JSValueConst new_target,
                                       int argc, JSValueConst *argv)
{
    return js_create_from_ctor(ctx, new_target, JS_CLASS_OBJECT);
}

static JSValue js_global_operators(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSValue func_obj, proto, opset_obj;

    func_obj = JS_UNDEFINED;
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return JS_EXCEPTION;
    opset_obj = js_operators_create_internal(ctx, argc, argv, FALSE);
    if (JS_IsException(opset_obj))
        goto fail;
    JS_DefinePropertyValue(ctx, proto, JS_ATOM_Symbol_operatorSet,
                           opset_obj, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    func_obj = JS_NewCFunction2(ctx, js_dummy_operators_ctor, "Operators",
                                0, JS_CFUNC_constructor, 0);
    if (JS_IsException(func_obj))
        goto fail;
    JS_SetConstructor2(ctx, func_obj, proto,
                       0, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, proto);
    return func_obj;
 fail:
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, func_obj);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_operators_funcs[] = {
    JS_CFUNC_DEF("create", 1, js_operators_create ),
    JS_CFUNC_DEF("updateBigIntOperators", 2, js_operators_updateBigIntOperators ),
};

/* must be called after all overloadable base types are initialized */
void JS_AddIntrinsicOperators(JSContext *ctx)
{
    JSValue obj;

    ctx->allow_operator_overloading = TRUE;
    obj = JS_NewCFunction(ctx, js_global_operators, "Operators", 1);
    JS_SetPropertyFunctionList(ctx, obj,
                               js_operators_funcs,
                               countof(js_operators_funcs));
    JS_DefinePropertyValue(ctx, ctx->global_obj, JS_ATOM_Operators,
                           obj,
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    /* add default operatorSets */
    js_operators_set_default(ctx, ctx->class_proto[JS_CLASS_BOOLEAN]);
    js_operators_set_default(ctx, ctx->class_proto[JS_CLASS_NUMBER]);
    js_operators_set_default(ctx, ctx->class_proto[JS_CLASS_STRING]);
    js_operators_set_default(ctx, ctx->class_proto[JS_CLASS_BIG_INT]);
    js_operators_set_default(ctx, ctx->class_proto[JS_CLASS_BIG_FLOAT]);
    js_operators_set_default(ctx, ctx->class_proto[JS_CLASS_BIG_DECIMAL]);
}

/* BigInt */

static JSValue JS_ToBigIntCtorFree(JSContext *ctx, JSValue val)
{
    uint32_t tag;

 redo:
    tag = JS_VALUE_GET_NORM_TAG(val);
    switch(tag) {
    case JS_TAG_INT:
    case JS_TAG_BOOL:
        val = JS_NewBigInt64(ctx, JS_VALUE_GET_INT(val));
        break;
    case JS_TAG_BIG_INT:
        break;
    case JS_TAG_FLOAT64:
    case JS_TAG_BIG_FLOAT:
        {
            bf_t *a, a_s;
            
            a = JS_ToBigFloat(ctx, &a_s, val);
            if (!bf_is_finite(a)) {
                JS_FreeValue(ctx, val);
                val = JS_ThrowRangeError(ctx, "cannot convert NaN or Infinity to bigint");
            } else {
                JSValue val1 = JS_NewBigInt(ctx);
                bf_t *r;
                int ret;
                if (JS_IsException(val1)) {
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
                r = JS_GetBigInt(val1);
                ret = bf_set(r, a);
                ret |= bf_rint(r, BF_RNDZ);
                JS_FreeValue(ctx, val);
                if (ret & BF_ST_MEM_ERROR) {
                    JS_FreeValue(ctx, val1);
                    val = JS_ThrowOutOfMemory(ctx);
                } else if (ret & BF_ST_INEXACT) {
                    JS_FreeValue(ctx, val1);
                    val = JS_ThrowRangeError(ctx, "cannot convert to bigint: not an integer");
                } else {
                    val = JS_CompactBigInt(ctx, val1);
                }
            }
            if (a == &a_s)
                bf_delete(a);
        }
        break;
    case JS_TAG_BIG_DECIMAL:
        val = JS_ToStringFree(ctx, val);
         if (JS_IsException(val))
            break;
        goto redo;
    case JS_TAG_STRING:
        val = JS_StringToBigIntErr(ctx, val);
        break;
    case JS_TAG_OBJECT:
        val = JS_ToPrimitiveFree(ctx, val, HINT_NUMBER);
        if (JS_IsException(val))
            break;
        goto redo;
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
    default:
        JS_FreeValue(ctx, val);
        return JS_ThrowTypeError(ctx, "cannot convert to bigint");
    }
    return val;
}

static JSValue js_bigint_constructor(JSContext *ctx,
                                     JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    if (!JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "not a constructor");
    return JS_ToBigIntCtorFree(ctx, JS_DupValue(ctx, argv[0]));
}

static JSValue js_thisBigIntValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsBigInt(ctx, this_val))
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BIG_INT) {
            if (JS_IsBigInt(ctx, p->u.object_data))
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a bigint");
}

static JSValue js_bigint_toString(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue val;
    int base;
    JSValue ret;

    val = js_thisBigIntValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        base = 10;
    } else {
        base = js_get_radix(ctx, argv[0]);
        if (base < 0)
            goto fail;
    }
    ret = js_bigint_to_string1(ctx, val, base);
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigint_valueOf(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    return js_thisBigIntValue(ctx, this_val);
}

static JSValue js_bigint_div(JSContext *ctx,
                              JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic)
{
    bf_t a_s, b_s, *a, *b, *r, *q;
    int status;
    JSValue q_val, r_val;
    
    q_val = JS_NewBigInt(ctx);
    if (JS_IsException(q_val))
        return JS_EXCEPTION;
    r_val = JS_NewBigInt(ctx);
    if (JS_IsException(r_val))
        goto fail;
    b = NULL;
    a = JS_ToBigInt(ctx, &a_s, argv[0]);
    if (!a)
        goto fail;
    b = JS_ToBigInt(ctx, &b_s, argv[1]);
    if (!b) {
        JS_FreeBigInt(ctx, a, &a_s);
        goto fail;
    }
    q = JS_GetBigInt(q_val);
    r = JS_GetBigInt(r_val);
    status = bf_divrem(q, r, a, b, BF_PREC_INF, BF_RNDZ, magic & 0xf);
    JS_FreeBigInt(ctx, a, &a_s);
    JS_FreeBigInt(ctx, b, &b_s);
    if (unlikely(status)) {
        throw_bf_exception(ctx, status);
        goto fail;
    }
    q_val = JS_CompactBigInt(ctx, q_val);
    if (magic & 0x10) {
        JSValue ret;
        ret = JS_NewArray(ctx);
        if (JS_IsException(ret))
            goto fail;
        JS_SetPropertyUint32(ctx, ret, 0, q_val);
        JS_SetPropertyUint32(ctx, ret, 1, JS_CompactBigInt(ctx, r_val));
        return ret;
    } else {
        JS_FreeValue(ctx, r_val);
        return q_val;
    }
 fail:
    JS_FreeValue(ctx, q_val);
    JS_FreeValue(ctx, r_val);
    return JS_EXCEPTION;
}

static JSValue js_bigint_sqrt(JSContext *ctx,
                               JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic)
{
    bf_t a_s, *a, *r, *rem;
    int status;
    JSValue r_val, rem_val;
    
    r_val = JS_NewBigInt(ctx);
    if (JS_IsException(r_val))
        return JS_EXCEPTION;
    rem_val = JS_NewBigInt(ctx);
    if (JS_IsException(rem_val))
        return JS_EXCEPTION;
    r = JS_GetBigInt(r_val);
    rem = JS_GetBigInt(rem_val);

    a = JS_ToBigInt(ctx, &a_s, argv[0]);
    if (!a)
        goto fail;
    status = bf_sqrtrem(r, rem, a);
    JS_FreeBigInt(ctx, a, &a_s);
    if (unlikely(status & ~BF_ST_INEXACT)) {
        throw_bf_exception(ctx, status);
        goto fail;
    }
    r_val = JS_CompactBigInt(ctx, r_val);
    if (magic) {
        JSValue ret;
        ret = JS_NewArray(ctx);
        if (JS_IsException(ret))
            goto fail;
        JS_SetPropertyUint32(ctx, ret, 0, r_val);
        JS_SetPropertyUint32(ctx, ret, 1, JS_CompactBigInt(ctx, rem_val));
        return ret;
    } else {
        JS_FreeValue(ctx, rem_val);
        return r_val;
    }
 fail:
    JS_FreeValue(ctx, r_val);
    JS_FreeValue(ctx, rem_val);
    return JS_EXCEPTION;
}

static JSValue js_bigint_op1(JSContext *ctx,
                              JSValueConst this_val,
                              int argc, JSValueConst *argv,
                              int magic)
{
    bf_t a_s, *a;
    int64_t res;

    a = JS_ToBigInt(ctx, &a_s, argv[0]);
    if (!a)
        return JS_EXCEPTION;
    switch(magic) {
    case 0: /* floorLog2 */
        if (a->sign || a->expn <= 0) {
            res = -1;
        } else {
            res = a->expn - 1;
        }
        break;
    case 1: /* ctz */
        if (bf_is_zero(a)) {
            res = -1;
        } else {
            res = bf_get_exp_min(a);
        }
        break;
    default:
        abort();
    }
    JS_FreeBigInt(ctx, a, &a_s);
    return JS_NewBigInt64(ctx, res);
}

static JSValue js_bigint_asUintN(JSContext *ctx,
                                  JSValueConst this_val,
                                  int argc, JSValueConst *argv, int asIntN)
{
    uint64_t bits;
    bf_t a_s, *a = &a_s, *r, mask_s, *mask = &mask_s;
    JSValue res;
    
    if (JS_ToIndex(ctx, &bits, argv[0]))
        return JS_EXCEPTION;
    res = JS_NewBigInt(ctx);
    if (JS_IsException(res))
        return JS_EXCEPTION;
    r = JS_GetBigInt(res);
    a = JS_ToBigInt(ctx, &a_s, argv[1]);
    if (!a) {
        JS_FreeValue(ctx, res);
        return JS_EXCEPTION;
    }
    /* XXX: optimize */
    r = JS_GetBigInt(res);
    bf_init(ctx->bf_ctx, mask);
    bf_set_ui(mask, 1);
    bf_mul_2exp(mask, bits, BF_PREC_INF, BF_RNDZ);
    bf_add_si(mask, mask, -1, BF_PREC_INF, BF_RNDZ);
    bf_logic_and(r, a, mask);
    if (asIntN && bits != 0) {
        bf_set_ui(mask, 1);
        bf_mul_2exp(mask, bits - 1, BF_PREC_INF, BF_RNDZ);
        if (bf_cmpu(r, mask) >= 0) {
            bf_set_ui(mask, 1);
            bf_mul_2exp(mask, bits, BF_PREC_INF, BF_RNDZ);
            bf_sub(r, r, mask, BF_PREC_INF, BF_RNDZ);
        }
    }
    bf_delete(mask);
    JS_FreeBigInt(ctx, a, &a_s);
    return JS_CompactBigInt(ctx, res);
}

static const JSCFunctionListEntry js_bigint_funcs[] = {
    JS_CFUNC_MAGIC_DEF("asUintN", 2, js_bigint_asUintN, 0 ),
    JS_CFUNC_MAGIC_DEF("asIntN", 2, js_bigint_asUintN, 1 ),
    /* QuickJS extensions */
    JS_CFUNC_MAGIC_DEF("tdiv", 2, js_bigint_div, BF_RNDZ ),
    JS_CFUNC_MAGIC_DEF("fdiv", 2, js_bigint_div, BF_RNDD ),
    JS_CFUNC_MAGIC_DEF("cdiv", 2, js_bigint_div, BF_RNDU ),
    JS_CFUNC_MAGIC_DEF("ediv", 2, js_bigint_div, BF_DIVREM_EUCLIDIAN ),
    JS_CFUNC_MAGIC_DEF("tdivrem", 2, js_bigint_div, BF_RNDZ | 0x10 ),
    JS_CFUNC_MAGIC_DEF("fdivrem", 2, js_bigint_div, BF_RNDD | 0x10 ),
    JS_CFUNC_MAGIC_DEF("cdivrem", 2, js_bigint_div, BF_RNDU | 0x10 ),
    JS_CFUNC_MAGIC_DEF("edivrem", 2, js_bigint_div, BF_DIVREM_EUCLIDIAN | 0x10 ),
    JS_CFUNC_MAGIC_DEF("sqrt", 1, js_bigint_sqrt, 0 ),
    JS_CFUNC_MAGIC_DEF("sqrtrem", 1, js_bigint_sqrt, 1 ),
    JS_CFUNC_MAGIC_DEF("floorLog2", 1, js_bigint_op1, 0 ),
    JS_CFUNC_MAGIC_DEF("ctz", 1, js_bigint_op1, 1 ),
};

static const JSCFunctionListEntry js_bigint_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_bigint_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_bigint_valueOf ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "BigInt", JS_PROP_CONFIGURABLE ),
};

void JS_AddIntrinsicBigInt(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValueConst obj1;

    rt->bigint_ops.to_string = js_bigint_to_string;
    rt->bigint_ops.from_string = js_string_to_bigint;
    rt->bigint_ops.unary_arith = js_unary_arith_bigint;
    rt->bigint_ops.binary_arith = js_binary_arith_bigint;
    rt->bigint_ops.compare = js_compare_bigfloat;
    
    ctx->class_proto[JS_CLASS_BIG_INT] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_BIG_INT],
                               js_bigint_proto_funcs,
                               countof(js_bigint_proto_funcs));
    obj1 = JS_NewGlobalCConstructor(ctx, "BigInt", js_bigint_constructor, 1,
                                    ctx->class_proto[JS_CLASS_BIG_INT]);
    JS_SetPropertyFunctionList(ctx, obj1, js_bigint_funcs,
                               countof(js_bigint_funcs));
}

/* BigFloat */

static JSValue js_thisBigFloatValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsBigFloat(this_val))
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BIG_FLOAT) {
            if (JS_IsBigFloat(p->u.object_data))
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a bigfloat");
}

static JSValue js_bigfloat_toString(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValue val;
    int base;
    JSValue ret;

    val = js_thisBigFloatValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        base = 10;
    } else {
        base = js_get_radix(ctx, argv[0]);
        if (base < 0)
            goto fail;
    }
    ret = js_ftoa(ctx, val, base, 0, BF_RNDN | BF_FTOA_FORMAT_FREE_MIN);
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigfloat_valueOf(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return js_thisBigFloatValue(ctx, this_val);
}

static int bigfloat_get_rnd_mode(JSContext *ctx, JSValueConst val)
{
    int rnd_mode;
    if (JS_ToInt32Sat(ctx, &rnd_mode, val))
        return -1;
    if (rnd_mode < BF_RNDN || rnd_mode > BF_RNDF) {
        JS_ThrowRangeError(ctx, "invalid rounding mode");
        return -1;
    }
    return rnd_mode;
}

static JSValue js_bigfloat_toFixed(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue val, ret;
    int64_t f;
    int rnd_mode, radix;

    val = js_thisBigFloatValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToInt64Sat(ctx, &f, argv[0]))
        goto fail;
    if (f < 0 || f > BF_PREC_MAX) {
        JS_ThrowRangeError(ctx, "invalid number of digits");
        goto fail;
    }
    rnd_mode = BF_RNDNA;
    radix = 10;
    /* XXX: swap parameter order for rounding mode and radix */
    if (argc > 1) {
        rnd_mode = bigfloat_get_rnd_mode(ctx, argv[1]);
        if (rnd_mode < 0)
            goto fail;
    }
    if (argc > 2) {
        radix = js_get_radix(ctx, argv[2]);
        if (radix < 0)
            goto fail;
    }
    ret = js_ftoa(ctx, val, radix, f, rnd_mode | BF_FTOA_FORMAT_FRAC);
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static BOOL js_bigfloat_is_finite(JSContext *ctx, JSValueConst val)
{
    BOOL res;
    uint32_t tag;

    tag = JS_VALUE_GET_NORM_TAG(val);
    switch(tag) {
    case JS_TAG_BIG_FLOAT:
        {
            JSBigFloat *p = JS_VALUE_GET_PTR(val);
            res = bf_is_finite(&p->num);
        }
        break;
    default:
        res = FALSE;
        break;
    }
    return res;
}

static JSValue js_bigfloat_toExponential(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue val, ret;
    int64_t f;
    int rnd_mode, radix;

    val = js_thisBigFloatValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToInt64Sat(ctx, &f, argv[0]))
        goto fail;
    if (!js_bigfloat_is_finite(ctx, val)) {
        ret = JS_ToString(ctx, val);
    } else if (JS_IsUndefined(argv[0])) {
        ret = js_ftoa(ctx, val, 10, 0,
                      BF_RNDN | BF_FTOA_FORMAT_FREE_MIN | BF_FTOA_FORCE_EXP);
    } else {
        if (f < 0 || f > BF_PREC_MAX) {
            JS_ThrowRangeError(ctx, "invalid number of digits");
            goto fail;
        }
        rnd_mode = BF_RNDNA;
        radix = 10;
        if (argc > 1) {
            rnd_mode = bigfloat_get_rnd_mode(ctx, argv[1]);
            if (rnd_mode < 0)
                goto fail;
        }
        if (argc > 2) {
            radix = js_get_radix(ctx, argv[2]);
            if (radix < 0)
                goto fail;
        }
        ret = js_ftoa(ctx, val, radix, f + 1,
                      rnd_mode | BF_FTOA_FORMAT_FIXED | BF_FTOA_FORCE_EXP);
    }
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigfloat_toPrecision(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val, ret;
    int64_t p;
    int rnd_mode, radix;

    val = js_thisBigFloatValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_IsUndefined(argv[0]))
        goto to_string;
    if (JS_ToInt64Sat(ctx, &p, argv[0]))
        goto fail;
    if (!js_bigfloat_is_finite(ctx, val)) {
    to_string:
        ret = JS_ToString(ctx, this_val);
    } else {
        if (p < 1 || p > BF_PREC_MAX) {
            JS_ThrowRangeError(ctx, "invalid number of digits");
            goto fail;
        }
        rnd_mode = BF_RNDNA;
        radix = 10;
        if (argc > 1) {
            rnd_mode = bigfloat_get_rnd_mode(ctx, argv[1]);
            if (rnd_mode < 0)
                goto fail;
        }
        if (argc > 2) {
            radix = js_get_radix(ctx, argv[2]);
            if (radix < 0)
                goto fail;
        }
        ret = js_ftoa(ctx, val, radix, p, rnd_mode | BF_FTOA_FORMAT_FIXED);
    }
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_bigfloat_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_bigfloat_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_bigfloat_valueOf ),
    JS_CFUNC_DEF("toPrecision", 1, js_bigfloat_toPrecision ),
    JS_CFUNC_DEF("toFixed", 1, js_bigfloat_toFixed ),
    JS_CFUNC_DEF("toExponential", 1, js_bigfloat_toExponential ),
};

static JSValue js_bigfloat_constructor(JSContext *ctx,
                                       JSValueConst new_target,
                                       int argc, JSValueConst *argv)
{
    JSValue val;
    if (!JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "not a constructor");
    if (argc == 0) {
        bf_t *r;
        val = JS_NewBigFloat(ctx);
        if (JS_IsException(val))
            return val;
        r = JS_GetBigFloat(val);
        bf_set_zero(r, 0);
    } else {
        val = JS_DupValue(ctx, argv[0]);
    redo:
        switch(JS_VALUE_GET_NORM_TAG(val)) {
        case JS_TAG_BIG_FLOAT:
            break;
        case JS_TAG_FLOAT64:
            {
                bf_t *r;
                double d = JS_VALUE_GET_FLOAT64(val);
                val = JS_NewBigFloat(ctx);
                if (JS_IsException(val))
                    break;
                r = JS_GetBigFloat(val);
                if (bf_set_float64(r, d))
                    goto fail;
            }
            break;
        case JS_TAG_INT:
            {
                bf_t *r;
                int32_t v = JS_VALUE_GET_INT(val);
                val = JS_NewBigFloat(ctx);
                if (JS_IsException(val))
                    break;
                r = JS_GetBigFloat(val);
                if (bf_set_si(r, v))
                    goto fail;
            }
            break;
        case JS_TAG_BIG_INT:
            /* We keep the full precision of the integer */
            {
                JSBigFloat *p = JS_VALUE_GET_PTR(val);
                val = JS_MKPTR(JS_TAG_BIG_FLOAT, p);
            }
            break;
        case JS_TAG_BIG_DECIMAL:
            val = JS_ToStringFree(ctx, val);
            if (JS_IsException(val))
                break;
            goto redo;
        case JS_TAG_STRING:
            {
                const char *str, *p;
                size_t len;
                int err;

                str = JS_ToCStringLen(ctx, &len, val);
                JS_FreeValue(ctx, val);
                if (!str)
                    return JS_EXCEPTION;
                p = str;
                p += skip_spaces(p);
                if ((p - str) == len) {
                    bf_t *r;
                    val = JS_NewBigFloat(ctx);
                    if (JS_IsException(val))
                        break;
                    r = JS_GetBigFloat(val);
                    bf_set_zero(r, 0);
                    err = 0;
                } else {
                    val = js_atof(ctx, p, &p, 0, ATOD_ACCEPT_BIN_OCT |
                                  ATOD_TYPE_BIG_FLOAT |
                                  ATOD_ACCEPT_PREFIX_AFTER_SIGN);
                    if (JS_IsException(val)) {
                        JS_FreeCString(ctx, str);
                        return JS_EXCEPTION;
                    }
                    p += skip_spaces(p);
                    err = ((p - str) != len);
                }
                JS_FreeCString(ctx, str);
                if (err) {
                    JS_FreeValue(ctx, val);
                    return JS_ThrowSyntaxError(ctx, "invalid bigfloat literal");
                }
            }
            break;
        case JS_TAG_OBJECT:
            val = JS_ToPrimitiveFree(ctx, val, HINT_NUMBER);
            if (JS_IsException(val))
                break;
            goto redo;
        case JS_TAG_NULL:
        case JS_TAG_UNDEFINED:
        default:
            JS_FreeValue(ctx, val);
            return JS_ThrowTypeError(ctx, "cannot convert to bigfloat");
        }
    }
    return val;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigfloat_get_const(JSContext *ctx,
                                     JSValueConst this_val, int magic)
{
    bf_t *r;
    JSValue val;
    val = JS_NewBigFloat(ctx);
    if (JS_IsException(val))
        return val;
    r = JS_GetBigFloat(val);
    switch(magic) {
    case 0: /* PI */
        bf_const_pi(r, ctx->fp_env.prec, ctx->fp_env.flags);
        break;
    case 1: /* LN2 */
        bf_const_log2(r, ctx->fp_env.prec, ctx->fp_env.flags);
        break;
    case 2: /* MIN_VALUE */
    case 3: /* MAX_VALUE */
        {
            slimb_t e_range, e;
            e_range = (limb_t)1 << (bf_get_exp_bits(ctx->fp_env.flags) - 1);
            bf_set_ui(r, 1);
            if (magic == 2) {
                e = -e_range + 2;
                if (ctx->fp_env.flags & BF_FLAG_SUBNORMAL)
                    e -= ctx->fp_env.prec - 1;
                bf_mul_2exp(r, e, ctx->fp_env.prec, ctx->fp_env.flags);
            } else {
                bf_mul_2exp(r, ctx->fp_env.prec, ctx->fp_env.prec,
                            ctx->fp_env.flags);
                bf_add_si(r, r, -1, ctx->fp_env.prec, ctx->fp_env.flags);
                bf_mul_2exp(r, e_range - ctx->fp_env.prec, ctx->fp_env.prec,
                            ctx->fp_env.flags);
            }
        }
        break;
    case 4: /* EPSILON */
        bf_set_ui(r, 1);
        bf_mul_2exp(r, 1 - ctx->fp_env.prec,
                    ctx->fp_env.prec, ctx->fp_env.flags);
        break;
    default:
        abort();
    }
    return val;
}

static JSValue js_bigfloat_parseFloat(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    bf_t *a;
    const char *str;
    JSValue ret;
    int radix;
    JSFloatEnv *fe;

    str = JS_ToCString(ctx, argv[0]);
    if (!str)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &radix, argv[1])) {
    fail:
        JS_FreeCString(ctx, str);
        return JS_EXCEPTION;
    }
    if (radix != 0 && (radix < 2 || radix > 36)) {
        JS_ThrowRangeError(ctx, "radix must be between 2 and 36");
        goto fail;
    }
    fe = &ctx->fp_env;
    if (argc > 2) {
        fe = JS_GetOpaque2(ctx, argv[2], JS_CLASS_FLOAT_ENV);
        if (!fe)
            goto fail;
    }
    ret = JS_NewBigFloat(ctx);
    if (JS_IsException(ret))
        goto done;
    a = JS_GetBigFloat(ret);
    /* XXX: use js_atof() */
    bf_atof(a, str, NULL, radix, fe->prec, fe->flags);
 done:
    JS_FreeCString(ctx, str);
    return ret;
}

static JSValue js_bigfloat_isFinite(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValueConst val = argv[0];
    JSBigFloat *p;
    
    if (JS_VALUE_GET_NORM_TAG(val) != JS_TAG_BIG_FLOAT)
        return JS_FALSE;
    p = JS_VALUE_GET_PTR(val);
    return JS_NewBool(ctx, bf_is_finite(&p->num));
}

static JSValue js_bigfloat_isNaN(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValueConst val = argv[0];
    JSBigFloat *p;
    
    if (JS_VALUE_GET_NORM_TAG(val) != JS_TAG_BIG_FLOAT)
        return JS_FALSE;
    p = JS_VALUE_GET_PTR(val);
    return JS_NewBool(ctx, bf_is_nan(&p->num));
}

enum {
    MATH_OP_ABS,
    MATH_OP_FLOOR,
    MATH_OP_CEIL,
    MATH_OP_ROUND,
    MATH_OP_TRUNC,
    MATH_OP_SQRT,
    MATH_OP_FPROUND,
    MATH_OP_ACOS,
    MATH_OP_ASIN,
    MATH_OP_ATAN,
    MATH_OP_ATAN2,
    MATH_OP_COS,
    MATH_OP_EXP,
    MATH_OP_LOG,
    MATH_OP_POW,
    MATH_OP_SIN,
    MATH_OP_TAN,
    MATH_OP_FMOD,
    MATH_OP_REM,
    MATH_OP_SIGN,

    MATH_OP_ADD,
    MATH_OP_SUB,
    MATH_OP_MUL,
    MATH_OP_DIV,
};

static JSValue js_bigfloat_fop(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic)
{
    bf_t a_s, *a, *r;
    JSFloatEnv *fe;
    int rnd_mode;
    JSValue op1, res;

    op1 = JS_ToNumeric(ctx, argv[0]);
    if (JS_IsException(op1))
        return op1;
    a = JS_ToBigFloat(ctx, &a_s, op1);
    fe = &ctx->fp_env;
    if (argc > 1) {
        fe = JS_GetOpaque2(ctx, argv[1], JS_CLASS_FLOAT_ENV);
        if (!fe)
            goto fail;
    }
    res = JS_NewBigFloat(ctx);
    if (JS_IsException(res)) {
    fail:
        if (a == &a_s)
            bf_delete(a);
        JS_FreeValue(ctx, op1);
        return JS_EXCEPTION;
    }
    r = JS_GetBigFloat(res);
    switch (magic) {
    case MATH_OP_ABS:
        bf_set(r, a);
        r->sign = 0;
        break;
    case MATH_OP_FLOOR:
        rnd_mode = BF_RNDD;
        goto rint;
    case MATH_OP_CEIL:
        rnd_mode = BF_RNDU;
        goto rint;
    case MATH_OP_ROUND:
        rnd_mode = BF_RNDNA;
        goto rint;
    case MATH_OP_TRUNC:
        rnd_mode = BF_RNDZ;
    rint:
        bf_set(r, a);
        fe->status |= bf_rint(r, rnd_mode);
        break;
    case MATH_OP_SQRT:
        fe->status |= bf_sqrt(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_FPROUND:
        bf_set(r, a);
        fe->status |= bf_round(r, fe->prec, fe->flags);
        break;
    case MATH_OP_ACOS:
        fe->status |= bf_acos(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_ASIN:
        fe->status |= bf_asin(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_ATAN:
        fe->status |= bf_atan(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_COS:
        fe->status |= bf_cos(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_EXP:
        fe->status |= bf_exp(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_LOG:
        fe->status |= bf_log(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_SIN:
        fe->status |= bf_sin(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_TAN:
        fe->status |= bf_tan(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_SIGN:
        if (bf_is_nan(a) || bf_is_zero(a)) {
            bf_set(r, a);
        } else {
            bf_set_si(r, 1 - 2 * a->sign);
        }
        break;
    default:
        abort();
    }
    if (a == &a_s)
        bf_delete(a);
    JS_FreeValue(ctx, op1);
    return res;
}

static JSValue js_bigfloat_fop2(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    bf_t a_s, *a, b_s, *b, r_s, *r = &r_s;
    JSFloatEnv *fe;
    JSValue op1, op2, res;

    op1 = JS_ToNumeric(ctx, argv[0]);
    if (JS_IsException(op1))
        return op1;
    op2 = JS_ToNumeric(ctx, argv[1]);
    if (JS_IsException(op2)) {
        JS_FreeValue(ctx, op1);
        return op2;
    }
    a = JS_ToBigFloat(ctx, &a_s, op1);
    b = JS_ToBigFloat(ctx, &b_s, op2);
    fe = &ctx->fp_env;
    if (argc > 2) {
        fe = JS_GetOpaque2(ctx, argv[2], JS_CLASS_FLOAT_ENV);
        if (!fe)
            goto fail;
    }
    res = JS_NewBigFloat(ctx);
    if (JS_IsException(res)) {
    fail:
        if (a == &a_s)
            bf_delete(a);
        if (b == &b_s)
            bf_delete(b);
        JS_FreeValue(ctx, op1);
        JS_FreeValue(ctx, op2);
        return JS_EXCEPTION;
    }
    r = JS_GetBigFloat(res);
    switch (magic) {
    case MATH_OP_ATAN2:
        fe->status |= bf_atan2(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_POW:
        fe->status |= bf_pow(r, a, b, fe->prec, fe->flags | BF_POW_JS_QUIRKS);
        break;
    case MATH_OP_FMOD:
        fe->status |= bf_rem(r, a, b, fe->prec, fe->flags, BF_RNDZ);
        break;
    case MATH_OP_REM:
        fe->status |= bf_rem(r, a, b, fe->prec, fe->flags, BF_RNDN);
        break;
    case MATH_OP_ADD:
        fe->status |= bf_add(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_SUB:
        fe->status |= bf_sub(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_MUL:
        fe->status |= bf_mul(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_DIV:
        fe->status |= bf_div(r, a, b, fe->prec, fe->flags);
        break;
    default:
        abort();
    }
    if (a == &a_s)
        bf_delete(a);
    if (b == &b_s)
        bf_delete(b);
    JS_FreeValue(ctx, op1);
    JS_FreeValue(ctx, op2);
    return res;
}

static const JSCFunctionListEntry js_bigfloat_funcs[] = {
    JS_CGETSET_MAGIC_DEF("PI", js_bigfloat_get_const, NULL, 0 ),
    JS_CGETSET_MAGIC_DEF("LN2", js_bigfloat_get_const, NULL, 1 ),
    JS_CGETSET_MAGIC_DEF("MIN_VALUE", js_bigfloat_get_const, NULL, 2 ),
    JS_CGETSET_MAGIC_DEF("MAX_VALUE", js_bigfloat_get_const, NULL, 3 ),
    JS_CGETSET_MAGIC_DEF("EPSILON", js_bigfloat_get_const, NULL, 4 ),
    JS_CFUNC_DEF("parseFloat", 1, js_bigfloat_parseFloat ),
    JS_CFUNC_DEF("isFinite", 1, js_bigfloat_isFinite ),
    JS_CFUNC_DEF("isNaN", 1, js_bigfloat_isNaN ),
    JS_CFUNC_MAGIC_DEF("abs", 1, js_bigfloat_fop, MATH_OP_ABS ),
    JS_CFUNC_MAGIC_DEF("fpRound", 1, js_bigfloat_fop, MATH_OP_FPROUND ),
    JS_CFUNC_MAGIC_DEF("floor", 1, js_bigfloat_fop, MATH_OP_FLOOR ),
    JS_CFUNC_MAGIC_DEF("ceil", 1, js_bigfloat_fop, MATH_OP_CEIL ),
    JS_CFUNC_MAGIC_DEF("round", 1, js_bigfloat_fop, MATH_OP_ROUND ),
    JS_CFUNC_MAGIC_DEF("trunc", 1, js_bigfloat_fop, MATH_OP_TRUNC ),
    JS_CFUNC_MAGIC_DEF("sqrt", 1, js_bigfloat_fop, MATH_OP_SQRT ),
    JS_CFUNC_MAGIC_DEF("acos", 1, js_bigfloat_fop, MATH_OP_ACOS ),
    JS_CFUNC_MAGIC_DEF("asin", 1, js_bigfloat_fop, MATH_OP_ASIN ),
    JS_CFUNC_MAGIC_DEF("atan", 1, js_bigfloat_fop, MATH_OP_ATAN ),
    JS_CFUNC_MAGIC_DEF("atan2", 2, js_bigfloat_fop2, MATH_OP_ATAN2 ),
    JS_CFUNC_MAGIC_DEF("cos", 1, js_bigfloat_fop, MATH_OP_COS ),
    JS_CFUNC_MAGIC_DEF("exp", 1, js_bigfloat_fop, MATH_OP_EXP ),
    JS_CFUNC_MAGIC_DEF("log", 1, js_bigfloat_fop, MATH_OP_LOG ),
    JS_CFUNC_MAGIC_DEF("pow", 2, js_bigfloat_fop2, MATH_OP_POW ),
    JS_CFUNC_MAGIC_DEF("sin", 1, js_bigfloat_fop, MATH_OP_SIN ),
    JS_CFUNC_MAGIC_DEF("tan", 1, js_bigfloat_fop, MATH_OP_TAN ),
    JS_CFUNC_MAGIC_DEF("sign", 1, js_bigfloat_fop, MATH_OP_SIGN ),
    JS_CFUNC_MAGIC_DEF("add", 2, js_bigfloat_fop2, MATH_OP_ADD ),
    JS_CFUNC_MAGIC_DEF("sub", 2, js_bigfloat_fop2, MATH_OP_SUB ),
    JS_CFUNC_MAGIC_DEF("mul", 2, js_bigfloat_fop2, MATH_OP_MUL ),
    JS_CFUNC_MAGIC_DEF("div", 2, js_bigfloat_fop2, MATH_OP_DIV ),
    JS_CFUNC_MAGIC_DEF("fmod", 2, js_bigfloat_fop2, MATH_OP_FMOD ),
    JS_CFUNC_MAGIC_DEF("remainder", 2, js_bigfloat_fop2, MATH_OP_REM ),
};

/* FloatEnv */

static JSValue js_float_env_constructor(JSContext *ctx,
                                        JSValueConst new_target,
                                        int argc, JSValueConst *argv)
{
    JSValue obj;
    JSFloatEnv *fe;
    int64_t prec;
    int flags, rndmode;

    prec = ctx->fp_env.prec;
    flags = ctx->fp_env.flags;
    if (!JS_IsUndefined(argv[0])) {
        if (JS_ToInt64Sat(ctx, &prec, argv[0]))
            return JS_EXCEPTION;
        if (prec < BF_PREC_MIN || prec > BF_PREC_MAX)
            return JS_ThrowRangeError(ctx, "invalid precision");
        flags = BF_RNDN; /* RNDN, max exponent size, no subnormal */
        if (argc > 1 && !JS_IsUndefined(argv[1])) {
            if (JS_ToInt32Sat(ctx, &rndmode, argv[1]))
                return JS_EXCEPTION;
            if (rndmode < BF_RNDN || rndmode > BF_RNDF)
                return JS_ThrowRangeError(ctx, "invalid rounding mode");
            flags = rndmode;
        }
    }

    obj = JS_NewObjectClass(ctx, JS_CLASS_FLOAT_ENV);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    fe = js_malloc(ctx, sizeof(*fe));
    if (!fe)
        return JS_EXCEPTION;
    fe->prec = prec;
    fe->flags = flags;
    fe->status = 0;
    JS_SetOpaque(obj, fe);
    return obj;
}

static void js_float_env_finalizer(JSRuntime *rt, JSValue val)
{
    JSFloatEnv *fe = JS_GetOpaque(val, JS_CLASS_FLOAT_ENV);
    js_free_rt(rt, fe);
}

static JSValue js_float_env_get_prec(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt64(ctx, ctx->fp_env.prec);
}

static JSValue js_float_env_get_expBits(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt32(ctx, bf_get_exp_bits(ctx->fp_env.flags));
}

static JSValue js_float_env_setPrec(JSContext *ctx,
                                    JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValueConst func;
    int exp_bits, flags, saved_flags;
    JSValue ret;
    limb_t saved_prec;
    int64_t prec;

    func = argv[0];
    if (JS_ToInt64Sat(ctx, &prec, argv[1]))
        return JS_EXCEPTION;
    if (prec < BF_PREC_MIN || prec > BF_PREC_MAX)
        return JS_ThrowRangeError(ctx, "invalid precision");
    exp_bits = BF_EXP_BITS_MAX;

    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32Sat(ctx, &exp_bits, argv[2]))
            return JS_EXCEPTION;
        if (exp_bits < BF_EXP_BITS_MIN || exp_bits > BF_EXP_BITS_MAX)
            return JS_ThrowRangeError(ctx, "invalid number of exponent bits");
    }

    flags = BF_RNDN | BF_FLAG_SUBNORMAL | bf_set_exp_bits(exp_bits);

    saved_prec = ctx->fp_env.prec;
    saved_flags = ctx->fp_env.flags;

    ctx->fp_env.prec = prec;
    ctx->fp_env.flags = flags;

    ret = JS_Call(ctx, func, JS_UNDEFINED, 0, NULL);
    /* always restore the floating point precision */
    ctx->fp_env.prec = saved_prec;
    ctx->fp_env.flags = saved_flags;
    return ret;
}

#define FE_PREC      (-1)
#define FE_EXP       (-2)
#define FE_RNDMODE   (-3)
#define FE_SUBNORMAL (-4)

static JSValue js_float_env_proto_get_status(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSFloatEnv *fe;
    fe = JS_GetOpaque2(ctx, this_val, JS_CLASS_FLOAT_ENV);
    if (!fe)
        return JS_EXCEPTION;
    switch(magic) {
    case FE_PREC:
        return JS_NewInt64(ctx, fe->prec);
    case FE_EXP:
        return JS_NewInt32(ctx, bf_get_exp_bits(fe->flags));
    case FE_RNDMODE:
        return JS_NewInt32(ctx, fe->flags & BF_RND_MASK);
    case FE_SUBNORMAL:
        return JS_NewBool(ctx, (fe->flags & BF_FLAG_SUBNORMAL) != 0);
    default:
        return JS_NewBool(ctx, (fe->status & magic) != 0);
    }
}

static JSValue js_float_env_proto_set_status(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSFloatEnv *fe;
    int b;
    int64_t prec;

    fe = JS_GetOpaque2(ctx, this_val, JS_CLASS_FLOAT_ENV);
    if (!fe)
        return JS_EXCEPTION;
    switch(magic) {
    case FE_PREC:
        if (JS_ToInt64Sat(ctx, &prec, val))
            return JS_EXCEPTION;
        if (prec < BF_PREC_MIN || prec > BF_PREC_MAX)
            return JS_ThrowRangeError(ctx, "invalid precision");
        fe->prec = prec;
        break;
    case FE_EXP:
        if (JS_ToInt32Sat(ctx, &b, val))
            return JS_EXCEPTION;
        if (b < BF_EXP_BITS_MIN || b > BF_EXP_BITS_MAX)
            return JS_ThrowRangeError(ctx, "invalid number of exponent bits");
        fe->flags = (fe->flags & ~(BF_EXP_BITS_MASK << BF_EXP_BITS_SHIFT)) |
            bf_set_exp_bits(b);
        break;
    case FE_RNDMODE:
        b = bigfloat_get_rnd_mode(ctx, val);
        if (b < 0)
            return JS_EXCEPTION;
        fe->flags = (fe->flags & ~BF_RND_MASK) | b;
        break;
    case FE_SUBNORMAL:
        b = JS_ToBool(ctx, val);
        fe->flags = (fe->flags & ~BF_FLAG_SUBNORMAL) | (b ? BF_FLAG_SUBNORMAL: 0);
        break;
    default:
        b = JS_ToBool(ctx, val);
        fe->status = (fe->status & ~magic) & ((-b) & magic);
        break;
    }
    return JS_UNDEFINED;
}

static JSValue js_float_env_clearStatus(JSContext *ctx,
                                        JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JSFloatEnv *fe = JS_GetOpaque2(ctx, this_val, JS_CLASS_FLOAT_ENV);
    if (!fe)
        return JS_EXCEPTION;
    fe->status = 0;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_float_env_funcs[] = {
    JS_CGETSET_DEF("prec", js_float_env_get_prec, NULL ),
    JS_CGETSET_DEF("expBits", js_float_env_get_expBits, NULL ),
    JS_CFUNC_DEF("setPrec", 2, js_float_env_setPrec ),
    JS_PROP_INT32_DEF("RNDN", BF_RNDN, 0 ),
    JS_PROP_INT32_DEF("RNDZ", BF_RNDZ, 0 ),
    JS_PROP_INT32_DEF("RNDU", BF_RNDU, 0 ),
    JS_PROP_INT32_DEF("RNDD", BF_RNDD, 0 ),
    JS_PROP_INT32_DEF("RNDNA", BF_RNDNA, 0 ),
    JS_PROP_INT32_DEF("RNDA", BF_RNDA, 0 ),
    JS_PROP_INT32_DEF("RNDF", BF_RNDF, 0 ),
    JS_PROP_INT32_DEF("precMin", BF_PREC_MIN, 0 ),
    JS_PROP_INT64_DEF("precMax", BF_PREC_MAX, 0 ),
    JS_PROP_INT32_DEF("expBitsMin", BF_EXP_BITS_MIN, 0 ),
    JS_PROP_INT32_DEF("expBitsMax", BF_EXP_BITS_MAX, 0 ),
};

static const JSCFunctionListEntry js_float_env_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("prec", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, FE_PREC ),
    JS_CGETSET_MAGIC_DEF("expBits", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, FE_EXP ),
    JS_CGETSET_MAGIC_DEF("rndMode", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, FE_RNDMODE ),
    JS_CGETSET_MAGIC_DEF("subnormal", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, FE_SUBNORMAL ),
    JS_CGETSET_MAGIC_DEF("invalidOperation", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, BF_ST_INVALID_OP ),
    JS_CGETSET_MAGIC_DEF("divideByZero", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, BF_ST_DIVIDE_ZERO ),
    JS_CGETSET_MAGIC_DEF("overflow", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, BF_ST_OVERFLOW ),
    JS_CGETSET_MAGIC_DEF("underflow", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, BF_ST_UNDERFLOW ),
    JS_CGETSET_MAGIC_DEF("inexact", js_float_env_proto_get_status,
                         js_float_env_proto_set_status, BF_ST_INEXACT ),
    JS_CFUNC_DEF("clearStatus", 0, js_float_env_clearStatus ),
};

void JS_AddIntrinsicBigFloat(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValueConst obj1;
    
    rt->bigfloat_ops.to_string = js_bigfloat_to_string;
    rt->bigfloat_ops.from_string = js_string_to_bigfloat;
    rt->bigfloat_ops.unary_arith = js_unary_arith_bigfloat;
    rt->bigfloat_ops.binary_arith = js_binary_arith_bigfloat;
    rt->bigfloat_ops.compare = js_compare_bigfloat;
    rt->bigfloat_ops.mul_pow10_to_float64 = js_mul_pow10_to_float64;
    rt->bigfloat_ops.mul_pow10 = js_mul_pow10;
    
    ctx->class_proto[JS_CLASS_BIG_FLOAT] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_BIG_FLOAT],
                               js_bigfloat_proto_funcs,
                               countof(js_bigfloat_proto_funcs));
    obj1 = JS_NewGlobalCConstructor(ctx, "BigFloat", js_bigfloat_constructor, 1,
                                    ctx->class_proto[JS_CLASS_BIG_FLOAT]);
    JS_SetPropertyFunctionList(ctx, obj1, js_bigfloat_funcs,
                               countof(js_bigfloat_funcs));

    ctx->class_proto[JS_CLASS_FLOAT_ENV] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_FLOAT_ENV],
                               js_float_env_proto_funcs,
                               countof(js_float_env_proto_funcs));
    obj1 = JS_NewGlobalCConstructorOnly(ctx, "BigFloatEnv",
                                        js_float_env_constructor, 1,
                                        ctx->class_proto[JS_CLASS_FLOAT_ENV]);
    JS_SetPropertyFunctionList(ctx, obj1, js_float_env_funcs,
                               countof(js_float_env_funcs));
}

/* BigDecimal */

static JSValue JS_ToBigDecimalFree(JSContext *ctx, JSValue val,
                                   BOOL allow_null_or_undefined)
{
 redo:
    switch(JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_BIG_DECIMAL:
        break;
    case JS_TAG_NULL:
        if (!allow_null_or_undefined)
            goto fail;
        /* fall thru */
    case JS_TAG_BOOL:
    case JS_TAG_INT:
        {
            bfdec_t *r;
            int32_t v = JS_VALUE_GET_INT(val);

            val = JS_NewBigDecimal(ctx);
            if (JS_IsException(val))
                break;
            r = JS_GetBigDecimal(val);
            if (bfdec_set_si(r, v)) {
                JS_FreeValue(ctx, val);
                val = JS_EXCEPTION;
                break;
            }
        }
        break;
    case JS_TAG_FLOAT64:
    case JS_TAG_BIG_INT:
    case JS_TAG_BIG_FLOAT:
        val = JS_ToStringFree(ctx, val);
        if (JS_IsException(val))
            break;
        goto redo;
    case JS_TAG_STRING:
        {
            const char *str, *p;
            size_t len;
            int err;

            str = JS_ToCStringLen(ctx, &len, val);
            JS_FreeValue(ctx, val);
            if (!str)
                return JS_EXCEPTION;
            p = str;
            p += skip_spaces(p);
            if ((p - str) == len) {
                bfdec_t *r;
                val = JS_NewBigDecimal(ctx);
                if (JS_IsException(val))
                    break;
                r = JS_GetBigDecimal(val);
                bfdec_set_zero(r, 0);
                err = 0;
            } else {
                val = js_atof(ctx, p, &p, 0, ATOD_TYPE_BIG_DECIMAL);
                if (JS_IsException(val)) {
                    JS_FreeCString(ctx, str);
                    return JS_EXCEPTION;
                }
                p += skip_spaces(p);
                err = ((p - str) != len);
            }
            JS_FreeCString(ctx, str);
            if (err) {
                JS_FreeValue(ctx, val);
                return JS_ThrowSyntaxError(ctx, "invalid bigdecimal literal");
            }
        }
        break;
    case JS_TAG_OBJECT:
        val = JS_ToPrimitiveFree(ctx, val, HINT_NUMBER);
        if (JS_IsException(val))
            break;
        goto redo;
    case JS_TAG_UNDEFINED:
        {
            bfdec_t *r;
            if (!allow_null_or_undefined)
                goto fail;
            val = JS_NewBigDecimal(ctx);
            if (JS_IsException(val))
                break;
            r = JS_GetBigDecimal(val);
            bfdec_set_nan(r);
        }
        break;
    default:
    fail:
        JS_FreeValue(ctx, val);
        return JS_ThrowTypeError(ctx, "cannot convert to bigdecimal");
    }
    return val;
}

static JSValue js_bigdecimal_constructor(JSContext *ctx,
                                         JSValueConst new_target,
                                         int argc, JSValueConst *argv)
{
    JSValue val;
    if (!JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "not a constructor");
    if (argc == 0) {
        bfdec_t *r;
        val = JS_NewBigDecimal(ctx);
        if (JS_IsException(val))
            return val;
        r = JS_GetBigDecimal(val);
        bfdec_set_zero(r, 0);
    } else {
        val = JS_ToBigDecimalFree(ctx, JS_DupValue(ctx, argv[0]), FALSE);
    }
    return val;
}

static JSValue js_thisBigDecimalValue(JSContext *ctx, JSValueConst this_val)
{
    if (JS_IsBigDecimal(this_val))
        return JS_DupValue(ctx, this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_BIG_DECIMAL) {
            if (JS_IsBigDecimal(p->u.object_data))
                return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a bigdecimal");
}

static JSValue js_bigdecimal_toString(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSValue val;

    val = js_thisBigDecimalValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    return JS_ToStringFree(ctx, val);
}

static JSValue js_bigdecimal_valueOf(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return js_thisBigDecimalValue(ctx, this_val);
}

static int js_bigdecimal_get_rnd_mode(JSContext *ctx, JSValueConst obj)
{
    const char *str;
    size_t size;
    int rnd_mode;
    
    str = JS_ToCStringLen(ctx, &size, obj);
    if (!str)
        return -1;
    if (strlen(str) != size)
        goto invalid_rounding_mode;
    if (!strcmp(str, "floor")) {
        rnd_mode = BF_RNDD;
    } else if (!strcmp(str, "ceiling")) {
        rnd_mode = BF_RNDU;
    } else if (!strcmp(str, "down")) {
        rnd_mode = BF_RNDZ;
    } else if (!strcmp(str, "up")) {
        rnd_mode = BF_RNDA;
    } else if (!strcmp(str, "half-even")) {
        rnd_mode = BF_RNDN;
    } else if (!strcmp(str, "half-up")) {
        rnd_mode = BF_RNDNA;
    } else {
    invalid_rounding_mode:
        JS_FreeCString(ctx, str);
        JS_ThrowTypeError(ctx, "invalid rounding mode");
        return -1;
    }
    JS_FreeCString(ctx, str);
    return rnd_mode;
}

typedef struct {
    int64_t prec;
    bf_flags_t flags;
} BigDecimalEnv;

static int js_bigdecimal_get_env(JSContext *ctx, BigDecimalEnv *fe,
                                 JSValueConst obj)
{
    JSValue prop;
    int64_t val;
    BOOL has_prec;
    int rnd_mode;
    
    if (!JS_IsObject(obj)) {
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    prop = JS_GetProperty(ctx, obj, JS_ATOM_roundingMode);
    if (JS_IsException(prop))
        return -1;
    rnd_mode = js_bigdecimal_get_rnd_mode(ctx, prop);
    JS_FreeValue(ctx, prop);
    if (rnd_mode < 0)
        return -1;
    fe->flags = rnd_mode;
    
    prop = JS_GetProperty(ctx, obj, JS_ATOM_maximumSignificantDigits);
    if (JS_IsException(prop))
        return -1;
    has_prec = FALSE;
    if (!JS_IsUndefined(prop)) {
        if (JS_ToInt64SatFree(ctx, &val, prop))
            return -1;
        if (val < 1 || val > BF_PREC_MAX)
            goto invalid_precision;
        fe->prec = val;
        has_prec = TRUE;
    }

    prop = JS_GetProperty(ctx, obj, JS_ATOM_maximumFractionDigits);
    if (JS_IsException(prop))
        return -1;
    if (!JS_IsUndefined(prop)) {
        if (has_prec) {
            JS_FreeValue(ctx, prop);
            JS_ThrowTypeError(ctx, "cannot provide both maximumSignificantDigits and maximumFractionDigits");
            return -1;
        }
        if (JS_ToInt64SatFree(ctx, &val, prop))
            return -1;
        if (val < 0 || val > BF_PREC_MAX) {
        invalid_precision:
            JS_ThrowTypeError(ctx, "invalid precision");
            return -1;
        }
        fe->prec = val;
        fe->flags |= BF_FLAG_RADPNT_PREC;
        has_prec = TRUE;
    }
    if (!has_prec) {
        JS_ThrowTypeError(ctx, "precision must be present");
        return -1;
    }
    return 0;
}


static JSValue js_bigdecimal_fop(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    bfdec_t *a, *b, r_s, *r = &r_s;
    JSValue op1, op2, res;
    BigDecimalEnv fe_s, *fe = &fe_s;
    int op_count, ret;

    if (magic == MATH_OP_SQRT ||
        magic == MATH_OP_ROUND)
        op_count = 1;
    else
        op_count = 2;
    
    op1 = JS_ToNumeric(ctx, argv[0]);
    if (JS_IsException(op1))
        return op1;
    a = JS_ToBigDecimal(ctx, op1);
    if (!a) {
        JS_FreeValue(ctx, op1);
        return JS_EXCEPTION;
    }
    if (op_count >= 2) {
        op2 = JS_ToNumeric(ctx, argv[1]);
        if (JS_IsException(op2)) {
            JS_FreeValue(ctx, op1);
            return op2;
        }
        b = JS_ToBigDecimal(ctx, op2);
        if (!b)
            goto fail;
    } else {
        op2 = JS_UNDEFINED;
        b = NULL;
    }
    fe->flags = BF_RNDZ;
    fe->prec = BF_PREC_INF;
    if (op_count < argc) {
        if (js_bigdecimal_get_env(ctx, fe, argv[op_count]))
            goto fail;
    }

    res = JS_NewBigDecimal(ctx);
    if (JS_IsException(res)) {
    fail:
        JS_FreeValue(ctx, op1);
        JS_FreeValue(ctx, op2);
        return JS_EXCEPTION;
    }
    r = JS_GetBigDecimal(res);
    switch (magic) {
    case MATH_OP_ADD:
        ret = bfdec_add(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_SUB:
        ret = bfdec_sub(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_MUL:
        ret = bfdec_mul(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_DIV:
        ret = bfdec_div(r, a, b, fe->prec, fe->flags);
        break;
    case MATH_OP_FMOD:
        ret = bfdec_rem(r, a, b, fe->prec, fe->flags, BF_RNDZ);
        break;
    case MATH_OP_SQRT:
        ret = bfdec_sqrt(r, a, fe->prec, fe->flags);
        break;
    case MATH_OP_ROUND:
        ret = bfdec_set(r, a);
        if (!(ret & BF_ST_MEM_ERROR))
            ret = bfdec_round(r, fe->prec, fe->flags);
        break;
    default:
        abort();
    }
    JS_FreeValue(ctx, op1);
    JS_FreeValue(ctx, op2);
    ret &= BF_ST_MEM_ERROR | BF_ST_DIVIDE_ZERO | BF_ST_INVALID_OP |
        BF_ST_OVERFLOW;
    if (ret != 0) {
        JS_FreeValue(ctx, res);
        return throw_bf_exception(ctx, ret);
    } else {
        return res;
    }
}

static JSValue js_bigdecimal_toFixed(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue val, ret;
    int64_t f;
    int rnd_mode;

    val = js_thisBigDecimalValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToInt64Sat(ctx, &f, argv[0]))
        goto fail;
    if (f < 0 || f > BF_PREC_MAX) {
        JS_ThrowRangeError(ctx, "invalid number of digits");
        goto fail;
    }
    rnd_mode = BF_RNDNA;
    if (argc > 1) {
        rnd_mode = js_bigdecimal_get_rnd_mode(ctx, argv[1]);
        if (rnd_mode < 0)
            goto fail;
    }
    ret = js_bigdecimal_to_string1(ctx, val, f, rnd_mode | BF_FTOA_FORMAT_FRAC);
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigdecimal_toExponential(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValue val, ret;
    int64_t f;
    int rnd_mode;

    val = js_thisBigDecimalValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_ToInt64Sat(ctx, &f, argv[0]))
        goto fail;
    if (JS_IsUndefined(argv[0])) {
        ret = js_bigdecimal_to_string1(ctx, val, 0,
                  BF_RNDN | BF_FTOA_FORMAT_FREE_MIN | BF_FTOA_FORCE_EXP);
    } else {
        if (f < 0 || f > BF_PREC_MAX) {
            JS_ThrowRangeError(ctx, "invalid number of digits");
            goto fail;
        }
        rnd_mode = BF_RNDNA;
        if (argc > 1) {
            rnd_mode = js_bigdecimal_get_rnd_mode(ctx, argv[1]);
            if (rnd_mode < 0)
                goto fail;
        }
        ret = js_bigdecimal_to_string1(ctx, val, f + 1,
                      rnd_mode | BF_FTOA_FORMAT_FIXED | BF_FTOA_FORCE_EXP);
    }
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static JSValue js_bigdecimal_toPrecision(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JSValue val, ret;
    int64_t p;
    int rnd_mode;

    val = js_thisBigDecimalValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (JS_IsUndefined(argv[0])) {
        return JS_ToStringFree(ctx, val);
    }
    if (JS_ToInt64Sat(ctx, &p, argv[0]))
        goto fail;
    if (p < 1 || p > BF_PREC_MAX) {
        JS_ThrowRangeError(ctx, "invalid number of digits");
        goto fail;
    }
    rnd_mode = BF_RNDNA;
    if (argc > 1) {
        rnd_mode = js_bigdecimal_get_rnd_mode(ctx, argv[1]);
        if (rnd_mode < 0)
            goto fail;
    }
    ret = js_bigdecimal_to_string1(ctx, val, p,
                                   rnd_mode | BF_FTOA_FORMAT_FIXED);
    JS_FreeValue(ctx, val);
    return ret;
 fail:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_bigdecimal_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_bigdecimal_toString ),
    JS_CFUNC_DEF("valueOf", 0, js_bigdecimal_valueOf ),
    JS_CFUNC_DEF("toPrecision", 1, js_bigdecimal_toPrecision ),
    JS_CFUNC_DEF("toFixed", 1, js_bigdecimal_toFixed ),
    JS_CFUNC_DEF("toExponential", 1, js_bigdecimal_toExponential ),
};

static const JSCFunctionListEntry js_bigdecimal_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 2, js_bigdecimal_fop, MATH_OP_ADD ),
    JS_CFUNC_MAGIC_DEF("sub", 2, js_bigdecimal_fop, MATH_OP_SUB ),
    JS_CFUNC_MAGIC_DEF("mul", 2, js_bigdecimal_fop, MATH_OP_MUL ),
    JS_CFUNC_MAGIC_DEF("div", 2, js_bigdecimal_fop, MATH_OP_DIV ),
    JS_CFUNC_MAGIC_DEF("mod", 2, js_bigdecimal_fop, MATH_OP_FMOD ),
    JS_CFUNC_MAGIC_DEF("round", 1, js_bigdecimal_fop, MATH_OP_ROUND ),
    JS_CFUNC_MAGIC_DEF("sqrt", 1, js_bigdecimal_fop, MATH_OP_SQRT ),
};

void JS_AddIntrinsicBigDecimal(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    JSValueConst obj1;

    rt->bigdecimal_ops.to_string = js_bigdecimal_to_string;
    rt->bigdecimal_ops.from_string = js_string_to_bigdecimal;
    rt->bigdecimal_ops.unary_arith = js_unary_arith_bigdecimal;
    rt->bigdecimal_ops.binary_arith = js_binary_arith_bigdecimal;
    rt->bigdecimal_ops.compare = js_compare_bigdecimal;

    ctx->class_proto[JS_CLASS_BIG_DECIMAL] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_BIG_DECIMAL],
                               js_bigdecimal_proto_funcs,
                               countof(js_bigdecimal_proto_funcs));
    obj1 = JS_NewGlobalCConstructor(ctx, "BigDecimal",
                                    js_bigdecimal_constructor, 1,
                                    ctx->class_proto[JS_CLASS_BIG_DECIMAL]);
    JS_SetPropertyFunctionList(ctx, obj1, js_bigdecimal_funcs,
                               countof(js_bigdecimal_funcs));
}

void JS_EnableBignumExt(JSContext *ctx, BOOL enable)
{
    ctx->bignum_ext = enable;
}

#endif /* CONFIG_BIGNUM */

static const char * const native_error_name[JS_NATIVE_ERROR_COUNT] = {
    "EvalError", "RangeError", "ReferenceError",
    "SyntaxError", "TypeError", "URIError",
    "InternalError", "AggregateError",
};

/* Minimum amount of objects to be able to compile code and display
   error messages. No JSAtom should be allocated by this function. */
static void JS_AddIntrinsicBasicObjects(JSContext *ctx)
{
    JSValue proto;
    int i;

    ctx->class_proto[JS_CLASS_OBJECT] = JS_NewObjectProto(ctx, JS_NULL);
    ctx->function_proto = JS_NewCFunction3(ctx, js_function_proto, "", 0,
                                           JS_CFUNC_generic, 0,
                                           ctx->class_proto[JS_CLASS_OBJECT]);
    ctx->class_proto[JS_CLASS_BYTECODE_FUNCTION] = JS_DupValue(ctx, ctx->function_proto);
    ctx->class_proto[JS_CLASS_ERROR] = JS_NewObject(ctx);
#if 0
    /* these are auto-initialized from js_error_proto_funcs,
       but delaying might be a problem */
    JS_DefinePropertyValue(ctx, ctx->class_proto[JS_CLASS_ERROR], JS_ATOM_name,
                           JS_AtomToString(ctx, JS_ATOM_Error),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, ctx->class_proto[JS_CLASS_ERROR], JS_ATOM_message,
                           JS_AtomToString(ctx, JS_ATOM_empty_string),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
#endif
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_ERROR],
                               js_error_proto_funcs,
                               countof(js_error_proto_funcs));

    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        proto = JS_NewObjectProto(ctx, ctx->class_proto[JS_CLASS_ERROR]);
        JS_DefinePropertyValue(ctx, proto, JS_ATOM_name,
                               JS_NewAtomString(ctx, native_error_name[i]),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_DefinePropertyValue(ctx, proto, JS_ATOM_message,
                               JS_AtomToString(ctx, JS_ATOM_empty_string),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        ctx->native_error_proto[i] = proto;
    }

    /* the array prototype is an array */
    ctx->class_proto[JS_CLASS_ARRAY] =
        JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                               JS_CLASS_ARRAY);

    ctx->array_shape = js_new_shape2(ctx, get_proto_obj(ctx->class_proto[JS_CLASS_ARRAY]),
                                     JS_PROP_INITIAL_HASH_SIZE, 1);
    add_shape_property(ctx, &ctx->array_shape, NULL,
                       JS_ATOM_length, JS_PROP_WRITABLE | JS_PROP_LENGTH);

    /* XXX: could test it on first context creation to ensure that no
       new atoms are created in JS_AddIntrinsicBasicObjects(). It is
       necessary to avoid useless renumbering of atoms after
       JS_EvalBinary() if it is done just after
       JS_AddIntrinsicBasicObjects(). */
    //    assert(ctx->rt->atom_count == JS_ATOM_END);
}

void JS_AddIntrinsicBaseObjects(JSContext *ctx)
{
    int i;
    JSValueConst obj, number_obj;
    JSValue obj1;

    ctx->throw_type_error = JS_NewCFunction(ctx, js_throw_type_error, NULL, 0);

    /* add caller and arguments properties to throw a TypeError */
    obj1 = JS_NewCFunction(ctx, js_function_proto_caller, NULL, 0);
    JS_DefineProperty(ctx, ctx->function_proto, JS_ATOM_caller, JS_UNDEFINED,
                      obj1, ctx->throw_type_error,
                      JS_PROP_HAS_GET | JS_PROP_HAS_SET |
                      JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE);
    JS_DefineProperty(ctx, ctx->function_proto, JS_ATOM_arguments, JS_UNDEFINED,
                      obj1, ctx->throw_type_error,
                      JS_PROP_HAS_GET | JS_PROP_HAS_SET |
                      JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, obj1);
    JS_FreeValue(ctx, js_object_seal(ctx, JS_UNDEFINED, 1, (JSValueConst *)&ctx->throw_type_error, 1));

    ctx->global_obj = JS_NewObject(ctx);
    ctx->global_var_obj = JS_NewObjectProto(ctx, JS_NULL);

    /* Object */
    obj = JS_NewGlobalCConstructor(ctx, "Object", js_object_constructor, 1,
                                   ctx->class_proto[JS_CLASS_OBJECT]);
    JS_SetPropertyFunctionList(ctx, obj, js_object_funcs, countof(js_object_funcs));
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                               js_object_proto_funcs, countof(js_object_proto_funcs));

    /* Function */
    JS_SetPropertyFunctionList(ctx, ctx->function_proto, js_function_proto_funcs, countof(js_function_proto_funcs));
    ctx->function_ctor = JS_NewCFunctionMagic(ctx, js_function_constructor,
                                              "Function", 1, JS_CFUNC_constructor_or_func_magic,
                                              JS_FUNC_NORMAL);
    JS_NewGlobalCConstructor2(ctx, JS_DupValue(ctx, ctx->function_ctor), "Function",
                              ctx->function_proto);

    /* Error */
    obj1 = JS_NewCFunctionMagic(ctx, js_error_constructor,
                                "Error", 1, JS_CFUNC_constructor_or_func_magic, -1);
    JS_NewGlobalCConstructor2(ctx, obj1,
                              "Error", ctx->class_proto[JS_CLASS_ERROR]);

    for(i = 0; i < JS_NATIVE_ERROR_COUNT; i++) {
        JSValue func_obj;
        int n_args;
        n_args = 1 + (i == JS_AGGREGATE_ERROR);
        func_obj = JS_NewCFunction3(ctx, (JSCFunction *)js_error_constructor,
                                    native_error_name[i], n_args,
                                    JS_CFUNC_constructor_or_func_magic, i, obj1);
        JS_NewGlobalCConstructor2(ctx, func_obj, native_error_name[i],
                                  ctx->native_error_proto[i]);
    }

    /* Iterator prototype */
    ctx->iterator_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->iterator_proto,
                               js_iterator_proto_funcs,
                               countof(js_iterator_proto_funcs));

    /* Array */
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_ARRAY],
                               js_array_proto_funcs,
                               countof(js_array_proto_funcs));

    obj = JS_NewGlobalCConstructor(ctx, "Array", js_array_constructor, 1,
                                   ctx->class_proto[JS_CLASS_ARRAY]);
    ctx->array_ctor = JS_DupValue(ctx, obj);
    JS_SetPropertyFunctionList(ctx, obj, js_array_funcs,
                               countof(js_array_funcs));

    /* XXX: create auto_initializer */
    {
        /* initialize Array.prototype[Symbol.unscopables] */
        char const unscopables[] = "copyWithin" "\0" "entries" "\0" "fill" "\0" "find" "\0"
            "findIndex" "\0" "flat" "\0" "flatMap" "\0" "includes" "\0" "keys" "\0" "values" "\0";
        const char *p = unscopables;
        obj1 = JS_NewObjectProto(ctx, JS_NULL);
        for(p = unscopables; *p; p += strlen(p) + 1) {
            JS_DefinePropertyValueStr(ctx, obj1, p, JS_TRUE, JS_PROP_C_W_E);
        }
        JS_DefinePropertyValue(ctx, ctx->class_proto[JS_CLASS_ARRAY],
                               JS_ATOM_Symbol_unscopables, obj1,
                               JS_PROP_CONFIGURABLE);
    }

    /* needed to initialize arguments[Symbol.iterator] */
    ctx->array_proto_values =
        JS_GetProperty(ctx, ctx->class_proto[JS_CLASS_ARRAY], JS_ATOM_values);

    ctx->class_proto[JS_CLASS_ARRAY_ITERATOR] = JS_NewObjectProto(ctx, ctx->iterator_proto);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_ARRAY_ITERATOR],
                               js_array_iterator_proto_funcs,
                               countof(js_array_iterator_proto_funcs));

    /* parseFloat and parseInteger must be defined before Number
       because of the Number.parseFloat and Number.parseInteger
       aliases */
    JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_global_funcs,
                               countof(js_global_funcs));

    /* Number */
    ctx->class_proto[JS_CLASS_NUMBER] = JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                                                               JS_CLASS_NUMBER);
    JS_SetObjectData(ctx, ctx->class_proto[JS_CLASS_NUMBER], JS_NewInt32(ctx, 0));
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_NUMBER],
                               js_number_proto_funcs,
                               countof(js_number_proto_funcs));
    number_obj = JS_NewGlobalCConstructor(ctx, "Number", js_number_constructor, 1,
                                          ctx->class_proto[JS_CLASS_NUMBER]);
    JS_SetPropertyFunctionList(ctx, number_obj, js_number_funcs, countof(js_number_funcs));

    /* Boolean */
    ctx->class_proto[JS_CLASS_BOOLEAN] = JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                                                                JS_CLASS_BOOLEAN);
    JS_SetObjectData(ctx, ctx->class_proto[JS_CLASS_BOOLEAN], JS_NewBool(ctx, FALSE));
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_BOOLEAN], js_boolean_proto_funcs,
                               countof(js_boolean_proto_funcs));
    JS_NewGlobalCConstructor(ctx, "Boolean", js_boolean_constructor, 1,
                             ctx->class_proto[JS_CLASS_BOOLEAN]);

    /* String */
    ctx->class_proto[JS_CLASS_STRING] = JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                                                               JS_CLASS_STRING);
    JS_SetObjectData(ctx, ctx->class_proto[JS_CLASS_STRING], JS_AtomToString(ctx, JS_ATOM_empty_string));
    obj = JS_NewGlobalCConstructor(ctx, "String", js_string_constructor, 1,
                                   ctx->class_proto[JS_CLASS_STRING]);
    JS_SetPropertyFunctionList(ctx, obj, js_string_funcs,
                               countof(js_string_funcs));
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_STRING], js_string_proto_funcs,
                               countof(js_string_proto_funcs));

    ctx->class_proto[JS_CLASS_STRING_ITERATOR] = JS_NewObjectProto(ctx, ctx->iterator_proto);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_STRING_ITERATOR],
                               js_string_iterator_proto_funcs,
                               countof(js_string_iterator_proto_funcs));

    /* Math: create as autoinit object */
    js_random_init(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_math_obj, countof(js_math_obj));

    /* ES6 Reflect: create as autoinit object */
    JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_reflect_obj, countof(js_reflect_obj));

    /* ES6 Symbol */
    ctx->class_proto[JS_CLASS_SYMBOL] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_SYMBOL], js_symbol_proto_funcs,
                               countof(js_symbol_proto_funcs));
    obj = JS_NewGlobalCConstructor(ctx, "Symbol", js_symbol_constructor, 0,
                                   ctx->class_proto[JS_CLASS_SYMBOL]);
    JS_SetPropertyFunctionList(ctx, obj, js_symbol_funcs,
                               countof(js_symbol_funcs));
    for(i = JS_ATOM_Symbol_toPrimitive; i < JS_ATOM_END; i++) {
        char buf[ATOM_GET_STR_BUF_SIZE];
        const char *str, *p;
        str = JS_AtomGetStr(ctx, buf, sizeof(buf), i);
        /* skip "Symbol." */
        p = strchr(str, '.');
        if (p)
            str = p + 1;
        JS_DefinePropertyValueStr(ctx, obj, str, JS_AtomToValue(ctx, i), 0);
    }

    /* ES6 Generator */
    ctx->class_proto[JS_CLASS_GENERATOR] = JS_NewObjectProto(ctx, ctx->iterator_proto);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_GENERATOR],
                               js_generator_proto_funcs,
                               countof(js_generator_proto_funcs));

    ctx->class_proto[JS_CLASS_GENERATOR_FUNCTION] = JS_NewObjectProto(ctx, ctx->function_proto);
    obj1 = JS_NewCFunctionMagic(ctx, js_function_constructor,
                                "GeneratorFunction", 1,
                                JS_CFUNC_constructor_or_func_magic, JS_FUNC_GENERATOR);
    JS_SetPropertyFunctionList(ctx,
                               ctx->class_proto[JS_CLASS_GENERATOR_FUNCTION],
                               js_generator_function_proto_funcs,
                               countof(js_generator_function_proto_funcs));
    JS_SetConstructor2(ctx, ctx->class_proto[JS_CLASS_GENERATOR_FUNCTION],
                       ctx->class_proto[JS_CLASS_GENERATOR],
                       JS_PROP_CONFIGURABLE, JS_PROP_CONFIGURABLE);
    JS_SetConstructor2(ctx, obj1, ctx->class_proto[JS_CLASS_GENERATOR_FUNCTION],
                       0, JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, obj1);

    /* global properties */
    ctx->eval_obj = JS_NewCFunction(ctx, js_global_eval, "eval", 1);
    JS_DefinePropertyValue(ctx, ctx->global_obj, JS_ATOM_eval,
                           JS_DupValue(ctx, ctx->eval_obj),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

    JS_DefinePropertyValue(ctx, ctx->global_obj, JS_ATOM_globalThis,
                           JS_DupValue(ctx, ctx->global_obj),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
}

/* Typed Arrays */

static uint8_t const typed_array_size_log2[JS_TYPED_ARRAY_COUNT] = {
    0, 0, 0, 1, 1, 2, 2,
#ifdef CONFIG_BIGNUM
    3, 3, /* BigInt64Array, BigUint64Array */
#endif
    2, 3
};

static JSValue js_array_buffer_constructor3(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len, JSClassID class_id,
                                            uint8_t *buf,
                                            JSFreeArrayBufferDataFunc *free_func,
                                            void *opaque, BOOL alloc_flag)
{
    JSRuntime *rt = ctx->rt;
    JSValue obj;
    JSArrayBuffer *abuf = NULL;

    obj = js_create_from_ctor(ctx, new_target, class_id);
    if (JS_IsException(obj))
        return obj;
    /* XXX: we are currently limited to 2 GB */
    if (len > INT32_MAX) {
        JS_ThrowRangeError(ctx, "invalid array buffer length");
        goto fail;
    }
    abuf = js_malloc(ctx, sizeof(*abuf));
    if (!abuf)
        goto fail;
    abuf->byte_length = len;
    if (alloc_flag) {
        if (class_id == JS_CLASS_SHARED_ARRAY_BUFFER &&
            rt->sab_funcs.sab_alloc) {
            abuf->data = rt->sab_funcs.sab_alloc(rt->sab_funcs.sab_opaque,
                                                 max_int(len, 1));
            if (!abuf->data)
                goto fail;
            memset(abuf->data, 0, len);
        } else {
            /* the allocation must be done after the object creation */
            abuf->data = js_mallocz(ctx, max_int(len, 1));
            if (!abuf->data)
                goto fail;
        }
    } else {
        if (class_id == JS_CLASS_SHARED_ARRAY_BUFFER &&
            rt->sab_funcs.sab_dup) {
            rt->sab_funcs.sab_dup(rt->sab_funcs.sab_opaque, buf);
        }
        abuf->data = buf;
    }
    init_list_head(&abuf->array_list);
    abuf->detached = FALSE;
    abuf->shared = (class_id == JS_CLASS_SHARED_ARRAY_BUFFER);
    abuf->opaque = opaque;
    abuf->free_func = free_func;
    if (alloc_flag && buf)
        memcpy(abuf->data, buf, len);
    JS_SetOpaque(obj, abuf);
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    js_free(ctx, abuf);
    return JS_EXCEPTION;
}

static void js_array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
    js_free_rt(rt, ptr);
}

static JSValue js_array_buffer_constructor2(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len, JSClassID class_id)
{
    return js_array_buffer_constructor3(ctx, new_target, len, class_id,
                                        NULL, js_array_buffer_free, NULL,
                                        TRUE);
}

static JSValue js_array_buffer_constructor1(JSContext *ctx,
                                            JSValueConst new_target,
                                            uint64_t len)
{
    return js_array_buffer_constructor2(ctx, new_target, len,
                                        JS_CLASS_ARRAY_BUFFER);
}

JSValue JS_NewArrayBuffer(JSContext *ctx, uint8_t *buf, size_t len,
                          JSFreeArrayBufferDataFunc *free_func, void *opaque,
                          BOOL is_shared)
{
    return js_array_buffer_constructor3(ctx, JS_UNDEFINED, len,
                                        is_shared ? JS_CLASS_SHARED_ARRAY_BUFFER : JS_CLASS_ARRAY_BUFFER,
                                        buf, free_func, opaque, FALSE);
}

/* create a new ArrayBuffer of length 'len' and copy 'buf' to it */
JSValue JS_NewArrayBufferCopy(JSContext *ctx, const uint8_t *buf, size_t len)
{
    return js_array_buffer_constructor3(ctx, JS_UNDEFINED, len,
                                        JS_CLASS_ARRAY_BUFFER,
                                        (uint8_t *)buf,
                                        js_array_buffer_free, NULL,
                                        TRUE);
}

static JSValue js_array_buffer_constructor(JSContext *ctx,
                                           JSValueConst new_target,
                                           int argc, JSValueConst *argv)
{
    uint64_t len;
    if (JS_ToIndex(ctx, &len, argv[0]))
        return JS_EXCEPTION;
    return js_array_buffer_constructor1(ctx, new_target, len);
}

static JSValue js_shared_array_buffer_constructor(JSContext *ctx,
                                                  JSValueConst new_target,
                                                  int argc, JSValueConst *argv)
{
    uint64_t len;
    if (JS_ToIndex(ctx, &len, argv[0]))
        return JS_EXCEPTION;
    return js_array_buffer_constructor2(ctx, new_target, len,
                                        JS_CLASS_SHARED_ARRAY_BUFFER);
}

/* also used for SharedArrayBuffer */
static void js_array_buffer_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSArrayBuffer *abuf = p->u.array_buffer;
    if (abuf) {
        /* The ArrayBuffer finalizer may be called before the typed
           array finalizers using it, so abuf->array_list is not
           necessarily empty. */
        // assert(list_empty(&abuf->array_list));
        if (abuf->shared && rt->sab_funcs.sab_free) {
            rt->sab_funcs.sab_free(rt->sab_funcs.sab_opaque, abuf->data);
        } else {
            if (abuf->free_func)
                abuf->free_func(rt, abuf->opaque, abuf->data);
        }
        js_free_rt(rt, abuf);
    }
}

static JSValue js_array_buffer_isView(JSContext *ctx,
                                      JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSObject *p;
    BOOL res;
    res = FALSE;
    if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(argv[0]);
        if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
            p->class_id <= JS_CLASS_DATAVIEW) {
            res = TRUE;
        }
    }
    return JS_NewBool(ctx, res);
}

static const JSCFunctionListEntry js_array_buffer_funcs[] = {
    JS_CFUNC_DEF("isView", 1, js_array_buffer_isView ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static JSValue JS_ThrowTypeErrorDetachedArrayBuffer(JSContext *ctx)
{
    return JS_ThrowTypeError(ctx, "ArrayBuffer is detached");
}

static JSValue js_array_buffer_get_byteLength(JSContext *ctx,
                                              JSValueConst this_val,
                                              int class_id)
{
    JSArrayBuffer *abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    /* return 0 if detached */
    return JS_NewUint32(ctx, abuf->byte_length);
}

void JS_DetachArrayBuffer(JSContext *ctx, JSValueConst obj)
{
    JSArrayBuffer *abuf = JS_GetOpaque(obj, JS_CLASS_ARRAY_BUFFER);
    struct list_head *el;

    if (!abuf || abuf->detached)
        return;
    if (abuf->free_func)
        abuf->free_func(ctx->rt, abuf->opaque, abuf->data);
    abuf->data = NULL;
    abuf->byte_length = 0;
    abuf->detached = TRUE;

    list_for_each(el, &abuf->array_list) {
        JSTypedArray *ta;
        JSObject *p;

        ta = list_entry(el, JSTypedArray, link);
        p = ta->obj;
        /* Note: the typed array length and offset fields are not modified */
        if (p->class_id != JS_CLASS_DATAVIEW) {
            p->u.array.count = 0;
            p->u.array.u.ptr = NULL;
        }
    }
}

/* get an ArrayBuffer or SharedArrayBuffer */
static JSArrayBuffer *js_get_array_buffer(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_ARRAY_BUFFER &&
        p->class_id != JS_CLASS_SHARED_ARRAY_BUFFER) {
    fail:
        JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_ARRAY_BUFFER);
        return NULL;
    }
    return p->u.array_buffer;
}

/* return NULL if exception. WARNING: any JS call can detach the
   buffer and render the returned pointer invalid */
uint8_t *JS_GetArrayBuffer(JSContext *ctx, size_t *psize, JSValueConst obj)
{
    JSArrayBuffer *abuf = js_get_array_buffer(ctx, obj);
    if (!abuf)
        goto fail;
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    *psize = abuf->byte_length;
    return abuf->data;
 fail:
    *psize = 0;
    return NULL;
}

static JSValue js_array_buffer_slice(JSContext *ctx,
                                     JSValueConst this_val,
                                     int argc, JSValueConst *argv, int class_id)
{
    JSArrayBuffer *abuf, *new_abuf;
    int64_t len, start, end, new_len;
    JSValue ctor, new_obj;

    abuf = JS_GetOpaque2(ctx, this_val, class_id);
    if (!abuf)
        return JS_EXCEPTION;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    len = abuf->byte_length;

    if (JS_ToInt64Clamp(ctx, &start, argv[0], 0, len, len))
        return JS_EXCEPTION;

    end = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt64Clamp(ctx, &end, argv[1], 0, len, len))
            return JS_EXCEPTION;
    }
    new_len = max_int64(end - start, 0);
    ctor = JS_SpeciesConstructor(ctx, this_val, JS_UNDEFINED);
    if (JS_IsException(ctor))
        return ctor;
    if (JS_IsUndefined(ctor)) {
        new_obj = js_array_buffer_constructor2(ctx, JS_UNDEFINED, new_len,
                                               class_id);
    } else {
        JSValue args[1];
        args[0] = JS_NewInt64(ctx, new_len);
        new_obj = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)args);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, args[0]);
    }
    if (JS_IsException(new_obj))
        return new_obj;
    new_abuf = JS_GetOpaque2(ctx, new_obj, class_id);
    if (!new_abuf)
        goto fail;
    if (js_same_value(ctx, new_obj, this_val)) {
        JS_ThrowTypeError(ctx, "cannot use identical ArrayBuffer");
        goto fail;
    }
    if (new_abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    if (new_abuf->byte_length < new_len) {
        JS_ThrowTypeError(ctx, "new ArrayBuffer is too small");
        goto fail;
    }
    /* must test again because of side effects */
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    memcpy(new_abuf->data, abuf->data + start, new_len);
    return new_obj;
 fail:
    JS_FreeValue(ctx, new_obj);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_array_buffer_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("byteLength", js_array_buffer_get_byteLength, NULL, JS_CLASS_ARRAY_BUFFER ),
    JS_CFUNC_MAGIC_DEF("slice", 2, js_array_buffer_slice, JS_CLASS_ARRAY_BUFFER ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "ArrayBuffer", JS_PROP_CONFIGURABLE ),
};

/* SharedArrayBuffer */

static const JSCFunctionListEntry js_shared_array_buffer_funcs[] = {
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_shared_array_buffer_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("byteLength", js_array_buffer_get_byteLength, NULL, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_CFUNC_MAGIC_DEF("slice", 2, js_array_buffer_slice, JS_CLASS_SHARED_ARRAY_BUFFER ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "SharedArrayBuffer", JS_PROP_CONFIGURABLE ),
};

static JSObject *get_typed_array(JSContext *ctx,
                                 JSValueConst this_val,
                                 int is_dataview)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(this_val);
    if (is_dataview) {
        if (p->class_id != JS_CLASS_DATAVIEW)
            goto fail;
    } else {
        if (!(p->class_id >= JS_CLASS_UINT8C_ARRAY &&
              p->class_id <= JS_CLASS_FLOAT64_ARRAY)) {
        fail:
            JS_ThrowTypeError(ctx, "not a %s", is_dataview ? "DataView" : "TypedArray");
            return NULL;
        }
    }
    return p;
}

/* WARNING: 'p' must be a typed array */
static BOOL typed_array_is_detached(JSContext *ctx, JSObject *p)
{
    JSTypedArray *ta = p->u.typed_array;
    JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
    /* XXX: could simplify test by ensuring that
       p->u.array.u.ptr is NULL iff it is detached */
    return abuf->detached;
}

/* WARNING: 'p' must be a typed array. Works even if the array buffer
   is detached */
static uint32_t typed_array_get_length(JSContext *ctx, JSObject *p)
{
    JSTypedArray *ta = p->u.typed_array;
    int size_log2 = typed_array_size_log2(p->class_id);
    return ta->length >> size_log2;
}

static int validate_typed_array(JSContext *ctx, JSValueConst this_val)
{
    JSObject *p;
    p = get_typed_array(ctx, this_val, 0);
    if (!p)
        return -1;
    if (typed_array_is_detached(ctx, p)) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        return -1;
    }
    return 0;
}

static JSValue js_typed_array_get_length(JSContext *ctx,
                                         JSValueConst this_val)
{
    JSObject *p;
    p = get_typed_array(ctx, this_val, 0);
    if (!p)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, p->u.array.count);
}

static JSValue js_typed_array_get_buffer(JSContext *ctx,
                                         JSValueConst this_val, int is_dataview)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, this_val, is_dataview);
    if (!p)
        return JS_EXCEPTION;
    ta = p->u.typed_array;
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}

static JSValue js_typed_array_get_byteLength(JSContext *ctx,
                                             JSValueConst this_val,
                                             int is_dataview)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, this_val, is_dataview);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_detached(ctx, p)) {
        if (is_dataview) {
            return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        } else {
            return JS_NewInt32(ctx, 0);
        }
    }
    ta = p->u.typed_array;
    return JS_NewInt32(ctx, ta->length);
}

static JSValue js_typed_array_get_byteOffset(JSContext *ctx,
                                             JSValueConst this_val,
                                             int is_dataview)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, this_val, is_dataview);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_detached(ctx, p)) {
        if (is_dataview) {
            return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        } else {
            return JS_NewInt32(ctx, 0);
        }
    }
    ta = p->u.typed_array;
    return JS_NewInt32(ctx, ta->offset);
}

/* Return the buffer associated to the typed array or an exception if
   it is not a typed array or if the buffer is detached. pbyte_offset,
   pbyte_length or pbytes_per_element can be NULL. */
JSValue JS_GetTypedArrayBuffer(JSContext *ctx, JSValueConst obj,
                               size_t *pbyte_offset,
                               size_t *pbyte_length,
                               size_t *pbytes_per_element)
{
    JSObject *p;
    JSTypedArray *ta;
    p = get_typed_array(ctx, obj, FALSE);
    if (!p)
        return JS_EXCEPTION;
    if (typed_array_is_detached(ctx, p))
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    ta = p->u.typed_array;
    if (pbyte_offset)
        *pbyte_offset = ta->offset;
    if (pbyte_length)
        *pbyte_length = ta->length;
    if (pbytes_per_element) {
        *pbytes_per_element = 1 << typed_array_size_log2(p->class_id);
    }
    return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
}
                               
static JSValue js_typed_array_get_toStringTag(JSContext *ctx,
                                              JSValueConst this_val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_UNDEFINED;
    p = JS_VALUE_GET_OBJ(this_val);
    if (!(p->class_id >= JS_CLASS_UINT8C_ARRAY &&
          p->class_id <= JS_CLASS_FLOAT64_ARRAY))
        return JS_UNDEFINED;
    return JS_AtomToString(ctx, ctx->rt->class_array[p->class_id].class_name);
}

static JSValue js_typed_array_set_internal(JSContext *ctx,
                                           JSValueConst dst,
                                           JSValueConst src,
                                           JSValueConst off)
{
    JSObject *p;
    JSObject *src_p;
    uint32_t i;
    int64_t src_len, offset;
    JSValue val, src_obj = JS_UNDEFINED;

    p = get_typed_array(ctx, dst, 0);
    if (!p)
        goto fail;
    if (JS_ToInt64Sat(ctx, &offset, off))
        goto fail;
    if (offset < 0)
        goto range_error;
    if (typed_array_is_detached(ctx, p)) {
    detached:
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    src_obj = JS_ToObject(ctx, src);
    if (JS_IsException(src_obj))
        goto fail;
    src_p = JS_VALUE_GET_OBJ(src_obj);
    if (src_p->class_id >= JS_CLASS_UINT8C_ARRAY &&
        src_p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
        JSTypedArray *dest_ta = p->u.typed_array;
        JSArrayBuffer *dest_abuf = dest_ta->buffer->u.array_buffer;
        JSTypedArray *src_ta = src_p->u.typed_array;
        JSArrayBuffer *src_abuf = src_ta->buffer->u.array_buffer;
        int shift = typed_array_size_log2(p->class_id);

        if (src_abuf->detached)
            goto detached;

        src_len = src_p->u.array.count;
        if (offset > (int64_t)(p->u.array.count - src_len))
            goto range_error;

        /* copying between typed objects */
        if (src_p->class_id == p->class_id) {
            /* same type, use memmove */
            memmove(dest_abuf->data + dest_ta->offset + (offset << shift),
                    src_abuf->data + src_ta->offset, src_len << shift);
            goto done;
        }
        if (dest_abuf->data == src_abuf->data) {
            /* copying between the same buffer using different types of mappings
               would require a temporary buffer */
        }
        /* otherwise, default behavior is slow but correct */
    } else {
        if (js_get_length64(ctx, &src_len, src_obj))
            goto fail;
        if (offset > (int64_t)(p->u.array.count - src_len)) {
        range_error:
            JS_ThrowRangeError(ctx, "invalid array length");
            goto fail;
        }
    }
    for(i = 0; i < src_len; i++) {
        val = JS_GetPropertyUint32(ctx, src_obj, i);
        if (JS_IsException(val))
            goto fail;
        if (JS_SetPropertyUint32(ctx, dst, offset + i, val) < 0)
            goto fail;
    }
done:
    JS_FreeValue(ctx, src_obj);
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, src_obj);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_set(JSContext *ctx,
                                  JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValueConst offset = JS_UNDEFINED;
    if (argc > 1) {
        offset = argv[1];
    }
    return js_typed_array_set_internal(ctx, this_val, argv[0], offset);
}

static JSValue js_create_typed_array_iterator(JSContext *ctx, JSValueConst this_val,
                                              int argc, JSValueConst *argv, int magic)
{
    if (validate_typed_array(ctx, this_val))
        return JS_EXCEPTION;
    return js_create_array_iterator(ctx, this_val, argc, argv, magic);
}

/* return < 0 if exception */
static int js_typed_array_get_length_internal(JSContext *ctx,
                                              JSValueConst obj)
{
    JSObject *p;
    p = get_typed_array(ctx, obj, 0);
    if (!p)
        return -1;
    if (typed_array_is_detached(ctx, p)) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        return -1;
    }
    return p->u.array.count;
}

#if 0
/* validate a typed array and return its length */
static JSValue js_typed_array___getLength(JSContext *ctx,
                                          JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    BOOL ignore_detached = JS_ToBool(ctx, argv[1]);

    if (ignore_detached) {
        return js_typed_array_get_length(ctx, argv[0]);
    } else {
        int len;
        len = js_typed_array_get_length_internal(ctx, argv[0]);
        if (len < 0)
            return JS_EXCEPTION;
        return JS_NewInt32(ctx, len);
    }
}
#endif

static JSValue js_typed_array_create(JSContext *ctx, JSValueConst ctor,
                                     int argc, JSValueConst *argv)
{
    JSValue ret;
    int new_len;
    int64_t len;

    ret = JS_CallConstructor(ctx, ctor, argc, argv);
    if (JS_IsException(ret))
        return ret;
    /* validate the typed array */
    new_len = js_typed_array_get_length_internal(ctx, ret);
    if (new_len < 0)
        goto fail;
    if (argc == 1) {
        /* ensure that it is large enough */
        if (JS_ToLengthFree(ctx, &len, JS_DupValue(ctx, argv[0])))
            goto fail;
        if (new_len < len) {
            JS_ThrowTypeError(ctx, "TypedArray length is too small");
        fail:
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
    }
    return ret;
}

#if 0
static JSValue js_typed_array___create(JSContext *ctx,
                                       JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    return js_typed_array_create(ctx, argv[0], max_int(argc - 1, 0), argv + 1);
}
#endif

static JSValue js_typed_array___speciesCreate(JSContext *ctx,
                                              JSValueConst this_val,
                                              int argc, JSValueConst *argv)
{
    JSValueConst obj;
    JSObject *p;
    JSValue ctor, ret;
    int argc1;

    obj = argv[0];
    p = get_typed_array(ctx, obj, 0);
    if (!p)
        return JS_EXCEPTION;
    ctor = JS_SpeciesConstructor(ctx, obj, JS_UNDEFINED);
    if (JS_IsException(ctor))
        return ctor;
    argc1 = max_int(argc - 1, 0);
    if (JS_IsUndefined(ctor)) {
        ret = js_typed_array_constructor(ctx, JS_UNDEFINED, argc1, argv + 1,
                                         p->class_id);
    } else {
        ret = js_typed_array_create(ctx, ctor, argc1, argv + 1);
        JS_FreeValue(ctx, ctor);
    }
    return ret;
}

static JSValue js_typed_array_from(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    // from(items, mapfn = void 0, this_arg = void 0)
    JSValueConst items = argv[0], mapfn, this_arg;
    JSValueConst args[2];
    JSValue stack[2];
    JSValue iter, arr, r, v, v2;
    int64_t k, len;
    int done, mapping;

    mapping = FALSE;
    mapfn = JS_UNDEFINED;
    this_arg = JS_UNDEFINED;
    r = JS_UNDEFINED;
    arr = JS_UNDEFINED;
    stack[0] = JS_UNDEFINED;
    stack[1] = JS_UNDEFINED;

    if (argc > 1) {
        mapfn = argv[1];
        if (!JS_IsUndefined(mapfn)) {
            if (check_function(ctx, mapfn))
                goto exception;
            mapping = 1;
            if (argc > 2)
                this_arg = argv[2];
        }
    }
    iter = JS_GetProperty(ctx, items, JS_ATOM_Symbol_iterator);
    if (JS_IsException(iter))
        goto exception;
    if (!JS_IsUndefined(iter)) {
        JS_FreeValue(ctx, iter);
        arr = JS_NewArray(ctx);
        if (JS_IsException(arr))
            goto exception;
        stack[0] = JS_DupValue(ctx, items);
        if (js_for_of_start(ctx, &stack[1], FALSE))
            goto exception;
        for (k = 0;; k++) {
            v = JS_IteratorNext(ctx, stack[0], stack[1], 0, NULL, &done);
            if (JS_IsException(v))
                goto exception_close;
            if (done)
                break;
            if (JS_DefinePropertyValueInt64(ctx, arr, k, v, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                goto exception_close;
        }
    } else {
        arr = JS_ToObject(ctx, items);
        if (JS_IsException(arr))
            goto exception;
    }
    if (js_get_length64(ctx, &len, arr) < 0)
        goto exception;
    v = JS_NewInt64(ctx, len);
    args[0] = v;
    r = js_typed_array_create(ctx, this_val, 1, args);
    JS_FreeValue(ctx, v);
    if (JS_IsException(r))
        goto exception;
    for(k = 0; k < len; k++) {
        v = JS_GetPropertyInt64(ctx, arr, k);
        if (JS_IsException(v))
            goto exception;
        if (mapping) {
            args[0] = v;
            args[1] = JS_NewInt32(ctx, k);
            v2 = JS_Call(ctx, mapfn, this_arg, 2, args);
            JS_FreeValue(ctx, v);
            v = v2;
            if (JS_IsException(v))
                goto exception;
        }
        if (JS_SetPropertyInt64(ctx, r, k, v) < 0)
            goto exception;
    }
    goto done;

 exception_close:
    if (!JS_IsUndefined(stack[0]))
        JS_IteratorClose(ctx, stack[0], TRUE);
 exception:
    JS_FreeValue(ctx, r);
    r = JS_EXCEPTION;
 done:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, stack[0]);
    JS_FreeValue(ctx, stack[1]);
    return r;
}

static JSValue js_typed_array_of(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValue obj;
    JSValueConst args[1];
    int i;

    args[0] = JS_NewInt32(ctx, argc);
    obj = js_typed_array_create(ctx, this_val, 1, args);
    if (JS_IsException(obj))
        return obj;

    for(i = 0; i < argc; i++) {
        if (JS_SetPropertyUint32(ctx, obj, i, JS_DupValue(ctx, argv[i])) < 0) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    return obj;
}

static JSValue js_typed_array_copyWithin(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JSObject *p;
    int len, to, from, final, count, shift;

    len = js_typed_array_get_length_internal(ctx, this_val);
    if (len < 0)
        return JS_EXCEPTION;

    if (JS_ToInt32Clamp(ctx, &to, argv[0], 0, len, len))
        return JS_EXCEPTION;

    if (JS_ToInt32Clamp(ctx, &from, argv[1], 0, len, len))
        return JS_EXCEPTION;

    final = len;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32Clamp(ctx, &final, argv[2], 0, len, len))
            return JS_EXCEPTION;
    }

    count = min_int(final - from, len - to);
    if (count > 0) {
        p = JS_VALUE_GET_OBJ(this_val);
        if (typed_array_is_detached(ctx, p))
            return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        shift = typed_array_size_log2(p->class_id);
        memmove(p->u.array.u.uint8_ptr + (to << shift),
                p->u.array.u.uint8_ptr + (from << shift),
                count << shift);
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_typed_array_fill(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSObject *p;
    int len, k, final, shift;
    uint64_t v64;

    len = js_typed_array_get_length_internal(ctx, this_val);
    if (len < 0)
        return JS_EXCEPTION;
    p = JS_VALUE_GET_OBJ(this_val);

    if (p->class_id == JS_CLASS_UINT8C_ARRAY) {
        int32_t v;
        if (JS_ToUint8ClampFree(ctx, &v, JS_DupValue(ctx, argv[0])))
            return JS_EXCEPTION;
        v64 = v;
    } else if (p->class_id <= JS_CLASS_UINT32_ARRAY) {
        uint32_t v;
        if (JS_ToUint32(ctx, &v, argv[0]))
            return JS_EXCEPTION;
        v64 = v;
    } else
#ifdef CONFIG_BIGNUM
    if (p->class_id <= JS_CLASS_BIG_UINT64_ARRAY) {
        if (JS_ToBigInt64(ctx, (int64_t *)&v64, argv[0]))
            return JS_EXCEPTION;
    } else
#endif
    {
        double d;
        if (JS_ToFloat64(ctx, &d, argv[0]))
            return JS_EXCEPTION;
        if (p->class_id == JS_CLASS_FLOAT32_ARRAY) {
            union {
                float f;
                uint32_t u32;
            } u;
            u.f = d;
            v64 = u.u32;
        } else {
            JSFloat64Union u;
            u.d = d;
            v64 = u.u64;
        }
    }

    k = 0;
    if (argc > 1) {
        if (JS_ToInt32Clamp(ctx, &k, argv[1], 0, len, len))
            return JS_EXCEPTION;
    }

    final = len;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32Clamp(ctx, &final, argv[2], 0, len, len))
            return JS_EXCEPTION;
    }

    if (typed_array_is_detached(ctx, p))
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    
    shift = typed_array_size_log2(p->class_id);
    switch(shift) {
    case 0:
        if (k < final) {
            memset(p->u.array.u.uint8_ptr + k, v64, final - k);
        }
        break;
    case 1:
        for(; k < final; k++) {
            p->u.array.u.uint16_ptr[k] = v64;
        }
        break;
    case 2:
        for(; k < final; k++) {
            p->u.array.u.uint32_ptr[k] = v64;
        }
        break;
    case 3:
        for(; k < final; k++) {
            p->u.array.u.uint64_ptr[k] = v64;
        }
        break;
    default:
        abort();
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_typed_array_find(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int findIndex)
{
    JSValueConst func, this_arg;
    JSValueConst args[3];
    JSValue val, index_val, res;
    int len, k;

    val = JS_UNDEFINED;
    len = js_typed_array_get_length_internal(ctx, this_val);
    if (len < 0)
        goto exception;

    func = argv[0];
    if (check_function(ctx, func))
        goto exception;

    this_arg = JS_UNDEFINED;
    if (argc > 1)
        this_arg = argv[1];

    for(k = 0; k < len; k++) {
        index_val = JS_NewInt32(ctx, k);
        val = JS_GetPropertyValue(ctx, this_val, index_val);
        if (JS_IsException(val))
            goto exception;
        args[0] = val;
        args[1] = index_val;
        args[2] = this_val;
        res = JS_Call(ctx, func, this_arg, 3, args);
        if (JS_IsException(res))
            goto exception;
        if (JS_ToBoolFree(ctx, res)) {
            if (findIndex) {
                JS_FreeValue(ctx, val);
                return index_val;
            } else {
                return val;
            }
        }
        JS_FreeValue(ctx, val);
    }
    if (findIndex)
        return JS_NewInt32(ctx, -1);
    else
        return JS_UNDEFINED;

exception:
    JS_FreeValue(ctx, val);
    return JS_EXCEPTION;
}

#define special_indexOf 0
#define special_lastIndexOf 1
#define special_includes -1

static JSValue js_typed_array_indexOf(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int special)
{
    JSObject *p;
    int len, tag, is_int, is_bigint, k, stop, inc, res = -1;
    int64_t v64;
    double d;
    float f;

    len = js_typed_array_get_length_internal(ctx, this_val);
    if (len < 0)
        goto exception;
    if (len == 0)
        goto done;

    if (special == special_lastIndexOf) {
        k = len - 1;
        if (argc > 1) {
            if (JS_ToFloat64(ctx, &d, argv[1]))
                goto exception;
            if (isnan(d)) {
                k = 0;
            } else {
                if (d >= 0) {
                    if (d < k) {
                        k = d;
                    }
                } else {
                    d += len;
                    if (d < 0)
                        goto done;
                    k = d;
                }
            }
        }
        stop = -1;
        inc = -1;
    } else {
        k = 0;
        if (argc > 1) {
            if (JS_ToInt32Clamp(ctx, &k, argv[1], 0, len, len))
                goto exception;
        }
        stop = len;
        inc = 1;
    }

    p = JS_VALUE_GET_OBJ(this_val);
    /* if the array was detached, no need to go further (but no
       exception is raised) */
    if (typed_array_is_detached(ctx, p)) {
        /* "includes" scans all the properties, so "undefined" can match */
        if (special == special_includes && JS_IsUndefined(argv[0]) && len > 0)
            res = 0;
        goto done;
    }
    
    is_bigint = 0;
    is_int = 0; /* avoid warning */
    v64 = 0; /* avoid warning */
    tag = JS_VALUE_GET_NORM_TAG(argv[0]);
    if (tag == JS_TAG_INT) {
        is_int = 1;
        v64 = JS_VALUE_GET_INT(argv[0]);
        d = v64;
    } else
    if (tag == JS_TAG_FLOAT64) {
        d = JS_VALUE_GET_FLOAT64(argv[0]);
        v64 = d;
        is_int = (v64 == d);
    } else
#ifdef CONFIG_BIGNUM
    if (tag == JS_TAG_BIG_INT) {
        JSBigFloat *p1 = JS_VALUE_GET_PTR(argv[0]);
        
        if (p->class_id == JS_CLASS_BIG_INT64_ARRAY) {
            if (bf_get_int64(&v64, &p1->num, 0) != 0)
                goto done;
        } else if (p->class_id == JS_CLASS_BIG_UINT64_ARRAY) {
            if (bf_get_uint64((uint64_t *)&v64, &p1->num) != 0)
                goto done;
        } else {
            goto done;
        }
        d = 0;
        is_bigint = 1;
    } else
#endif
    {
        goto done;
    }

    switch (p->class_id) {
    case JS_CLASS_INT8_ARRAY:
        if (is_int && (int8_t)v64 == v64)
            goto scan8;
        break;
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
        if (is_int && (uint8_t)v64 == v64) {
            const uint8_t *pv, *pp;
            uint16_t v;
        scan8:
            pv = p->u.array.u.uint8_ptr;
            v = v64;
            if (inc > 0) {
                pp = memchr(pv + k, v, len - k);
                if (pp)
                    res = pp - pv;
            } else {
                for (; k != stop; k += inc) {
                    if (pv[k] == v) {
                        res = k;
                        break;
                    }
                }
            }
        }
        break;
    case JS_CLASS_INT16_ARRAY:
        if (is_int && (int16_t)v64 == v64)
            goto scan16;
        break;
    case JS_CLASS_UINT16_ARRAY:
        if (is_int && (uint16_t)v64 == v64) {
            const uint16_t *pv;
            uint16_t v;
        scan16:
            pv = p->u.array.u.uint16_ptr;
            v = v64;
            for (; k != stop; k += inc) {
                if (pv[k] == v) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_INT32_ARRAY:
        if (is_int && (int32_t)v64 == v64)
            goto scan32;
        break;
    case JS_CLASS_UINT32_ARRAY:
        if (is_int && (uint32_t)v64 == v64) {
            const uint32_t *pv;
            uint32_t v;
        scan32:
            pv = p->u.array.u.uint32_ptr;
            v = v64;
            for (; k != stop; k += inc) {
                if (pv[k] == v) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_FLOAT32_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            const float *pv = p->u.array.u.float_ptr;
            /* special case: indexOf returns -1, includes finds NaN */
            if (special != special_includes)
                goto done;
            for (; k != stop; k += inc) {
                if (isnan(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else if ((f = (float)d) == d) {
            const float *pv = p->u.array.u.float_ptr;
            for (; k != stop; k += inc) {
                if (pv[k] == f) {
                    res = k;
                    break;
                }
            }
        }
        break;
    case JS_CLASS_FLOAT64_ARRAY:
        if (is_bigint)
            break;
        if (isnan(d)) {
            const double *pv = p->u.array.u.double_ptr;
            /* special case: indexOf returns -1, includes finds NaN */
            if (special != special_includes)
                goto done;
            for (; k != stop; k += inc) {
                if (isnan(pv[k])) {
                    res = k;
                    break;
                }
            }
        } else {
            const double *pv = p->u.array.u.double_ptr;
            for (; k != stop; k += inc) {
                if (pv[k] == d) {
                    res = k;
                    break;
                }
            }
        }
        break;
#ifdef CONFIG_BIGNUM
    case JS_CLASS_BIG_INT64_ARRAY:
        if (is_bigint || (is_math_mode(ctx) && is_int &&
                          v64 >= -MAX_SAFE_INTEGER &&
                          v64 <= MAX_SAFE_INTEGER)) {
            goto scan64;
        }
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        if (is_bigint || (is_math_mode(ctx) && is_int &&
                          v64 >= 0 && v64 <= MAX_SAFE_INTEGER)) {
            const uint64_t *pv;
            uint64_t v;
        scan64:
            pv = p->u.array.u.uint64_ptr;
            v = v64;
            for (; k != stop; k += inc) {
                if (pv[k] == v) {
                    res = k;
                    break;
                }
            }
        }
        break;
#endif
    }

done:
    if (special == special_includes)
        return JS_NewBool(ctx, res >= 0);
    else
        return JS_NewInt32(ctx, res);

exception:
    return JS_EXCEPTION;
}

static JSValue js_typed_array_join(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv, int toLocaleString)
{
    JSValue sep = JS_UNDEFINED, el;
    StringBuffer b_s, *b = &b_s;
    JSString *p = NULL;
    int i, n;
    int c;

    n = js_typed_array_get_length_internal(ctx, this_val);
    if (n < 0)
        goto exception;

    c = ',';    /* default separator */
    if (!toLocaleString && argc > 0 && !JS_IsUndefined(argv[0])) {
        sep = JS_ToString(ctx, argv[0]);
        if (JS_IsException(sep))
            goto exception;
        p = JS_VALUE_GET_STRING(sep);
        if (p->len == 1 && !p->is_wide_char)
            c = p->u.str8[0];
        else
            c = -1;
    }
    string_buffer_init(ctx, b, 0);

    /* XXX: optimize with direct access */
    for(i = 0; i < n; i++) {
        if (i > 0) {
            if (c >= 0) {
                if (string_buffer_putc8(b, c))
                    goto fail;
            } else {
                if (string_buffer_concat(b, p, 0, p->len))
                    goto fail;
            }
        }
        el = JS_GetPropertyUint32(ctx, this_val, i);
        /* Can return undefined for example if the typed array is detached */
        if (!JS_IsNull(el) && !JS_IsUndefined(el)) {
            if (JS_IsException(el))
                goto fail;
            if (toLocaleString) {
                el = JS_ToLocaleStringFree(ctx, el);
            }
            if (string_buffer_concat_value_free(b, el))
                goto fail;
        }
    }
    JS_FreeValue(ctx, sep);
    return string_buffer_end(b);

fail:
    string_buffer_free(b);
    JS_FreeValue(ctx, sep);
exception:
    return JS_EXCEPTION;
}

static JSValue js_typed_array_reverse(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JSObject *p;
    int len;

    len = js_typed_array_get_length_internal(ctx, this_val);
    if (len < 0)
        return JS_EXCEPTION;
    if (len > 0) {
        p = JS_VALUE_GET_OBJ(this_val);
        switch (typed_array_size_log2(p->class_id)) {
        case 0:
            {
                uint8_t *p1 = p->u.array.u.uint8_ptr;
                uint8_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint8_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 1:
            {
                uint16_t *p1 = p->u.array.u.uint16_ptr;
                uint16_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint16_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 2:
            {
                uint32_t *p1 = p->u.array.u.uint32_ptr;
                uint32_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint32_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        case 3:
            {
                uint64_t *p1 = p->u.array.u.uint64_ptr;
                uint64_t *p2 = p1 + len - 1;
                while (p1 < p2) {
                    uint64_t v = *p1;
                    *p1++ = *p2;
                    *p2-- = v;
                }
            }
            break;
        default:
            abort();
        }
    }
    return JS_DupValue(ctx, this_val);
}

static JSValue js_typed_array_slice(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JSValueConst args[2];
    JSValue arr, val;
    JSObject *p, *p1;
    int n, len, start, final, count, shift;

    arr = JS_UNDEFINED;
    len = js_typed_array_get_length_internal(ctx, this_val);
    if (len < 0)
        goto exception;

    if (JS_ToInt32Clamp(ctx, &start, argv[0], 0, len, len))
        goto exception;
    final = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &final, argv[1], 0, len, len))
            goto exception;
    }
    count = max_int(final - start, 0);

    p = get_typed_array(ctx, this_val, 0);
    if (p == NULL)
        goto exception;
    shift = typed_array_size_log2(p->class_id);

    args[0] = this_val;
    args[1] = JS_NewInt32(ctx, count);
    arr = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 2, args);
    if (JS_IsException(arr))
        goto exception;

    if (count > 0) {
        if (validate_typed_array(ctx, this_val)
        ||  validate_typed_array(ctx, arr))
            goto exception;

        p1 = get_typed_array(ctx, arr, 0);
        if (p1 != NULL && p->class_id == p1->class_id &&
            typed_array_get_length(ctx, p1) >= count &&
            typed_array_get_length(ctx, p) >= start + count) {
            memcpy(p1->u.array.u.uint8_ptr,
                   p->u.array.u.uint8_ptr + (start << shift),
                   count << shift);
        } else {
            for (n = 0; n < count; n++) {
                val = JS_GetPropertyValue(ctx, this_val, JS_NewInt32(ctx, start + n));
                if (JS_IsException(val))
                    goto exception;
                if (JS_SetPropertyValue(ctx, arr, JS_NewInt32(ctx, n), val,
                                        JS_PROP_THROW) < 0)
                    goto exception;
            }
        }
    }
    return arr;

 exception:
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_subarray(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSValueConst args[4];
    JSValue arr, byteOffset, ta_buffer;
    JSObject *p;
    int len, start, final, count, shift, offset;

    p = get_typed_array(ctx, this_val, 0);
    if (!p)
        goto exception;
    len = p->u.array.count;
    if (JS_ToInt32Clamp(ctx, &start, argv[0], 0, len, len))
        goto exception;

    final = len;
    if (!JS_IsUndefined(argv[1])) {
        if (JS_ToInt32Clamp(ctx, &final, argv[1], 0, len, len))
            goto exception;
    }
    count = max_int(final - start, 0);
    byteOffset = js_typed_array_get_byteOffset(ctx, this_val, 0);
    if (JS_IsException(byteOffset))
        goto exception;
    shift = typed_array_size_log2(p->class_id);
    offset = JS_VALUE_GET_INT(byteOffset) + (start << shift);
    JS_FreeValue(ctx, byteOffset);
    ta_buffer = js_typed_array_get_buffer(ctx, this_val, 0);
    if (JS_IsException(ta_buffer))
        goto exception;
    args[0] = this_val;
    args[1] = ta_buffer;
    args[2] = JS_NewInt32(ctx, offset);
    args[3] = JS_NewInt32(ctx, count);
    arr = js_typed_array___speciesCreate(ctx, JS_UNDEFINED, 4, args);
    JS_FreeValue(ctx, ta_buffer);
    return arr;

 exception:
    return JS_EXCEPTION;
}

/* TypedArray.prototype.sort */

static int js_cmp_doubles(double x, double y)
{
    if (isnan(x))    return isnan(y) ? 0 : +1;
    if (isnan(y))    return -1;
    if (x < y)       return -1;
    if (x > y)       return 1;
    if (x != 0)      return 0;
    if (signbit(x))  return signbit(y) ? 0 : -1;
    else             return signbit(y) ? 1 : 0;
}

static int js_TA_cmp_int8(const void *a, const void *b, void *opaque) {
    return *(const int8_t *)a - *(const int8_t *)b;
}

static int js_TA_cmp_uint8(const void *a, const void *b, void *opaque) {
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

static int js_TA_cmp_int16(const void *a, const void *b, void *opaque) {
    return *(const int16_t *)a - *(const int16_t *)b;
}

static int js_TA_cmp_uint16(const void *a, const void *b, void *opaque) {
    return *(const uint16_t *)a - *(const uint16_t *)b;
}

static int js_TA_cmp_int32(const void *a, const void *b, void *opaque) {
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_uint32(const void *a, const void *b, void *opaque) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (y < x) - (y > x);
}

#ifdef CONFIG_BIGNUM
static int js_TA_cmp_int64(const void *a, const void *b, void *opaque) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (y < x) - (y > x);
}

static int js_TA_cmp_uint64(const void *a, const void *b, void *opaque) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (y < x) - (y > x);
}
#endif

static int js_TA_cmp_float32(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(*(const float *)a, *(const float *)b);
}

static int js_TA_cmp_float64(const void *a, const void *b, void *opaque) {
    return js_cmp_doubles(*(const double *)a, *(const double *)b);
}

static JSValue js_TA_get_int8(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const int8_t *)a);
}

static JSValue js_TA_get_uint8(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const uint8_t *)a);
}

static JSValue js_TA_get_int16(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const int16_t *)a);
}

static JSValue js_TA_get_uint16(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const uint16_t *)a);
}

static JSValue js_TA_get_int32(JSContext *ctx, const void *a) {
    return JS_NewInt32(ctx, *(const int32_t *)a);
}

static JSValue js_TA_get_uint32(JSContext *ctx, const void *a) {
    return JS_NewUint32(ctx, *(const uint32_t *)a);
}

#ifdef CONFIG_BIGNUM
static JSValue js_TA_get_int64(JSContext *ctx, const void *a) {
    return JS_NewBigInt64(ctx, *(int64_t *)a);
}

static JSValue js_TA_get_uint64(JSContext *ctx, const void *a) {
    return JS_NewBigUint64(ctx, *(uint64_t *)a);
}
#endif

static JSValue js_TA_get_float32(JSContext *ctx, const void *a) {
    return __JS_NewFloat64(ctx, *(const float *)a);
}

static JSValue js_TA_get_float64(JSContext *ctx, const void *a) {
    return __JS_NewFloat64(ctx, *(const double *)a);
}

struct TA_sort_context {
    JSContext *ctx;
    int exception;
    JSValueConst arr;
    JSValueConst cmp;
    JSValue (*getfun)(JSContext *ctx, const void *a);
    uint8_t *array_ptr; /* cannot change unless the array is detached */
    int elt_size;
};

static int js_TA_cmp_generic(const void *a, const void *b, void *opaque) {
    struct TA_sort_context *psc = opaque;
    JSContext *ctx = psc->ctx;
    uint32_t a_idx, b_idx;
    JSValueConst argv[2];
    JSValue res;
    int cmp;

    cmp = 0;
    if (!psc->exception) {
        a_idx = *(uint32_t *)a;
        b_idx = *(uint32_t *)b;
        argv[0] = psc->getfun(ctx, psc->array_ptr +
                              a_idx * (size_t)psc->elt_size);
        argv[1] = psc->getfun(ctx, psc->array_ptr +
                              b_idx * (size_t)(psc->elt_size));
        res = JS_Call(ctx, psc->cmp, JS_UNDEFINED, 2, argv);
        if (JS_IsException(res)) {
            psc->exception = 1;
            goto done;
        }
        if (JS_VALUE_GET_TAG(res) == JS_TAG_INT) {
            int val = JS_VALUE_GET_INT(res);
            cmp = (val > 0) - (val < 0);
        } else {
            double val;
            if (JS_ToFloat64Free(ctx, &val, res) < 0) {
                psc->exception = 1;
                goto done;
            } else {
                cmp = (val > 0) - (val < 0);
            }
        }
        if (cmp == 0) {
            /* make sort stable: compare array offsets */
            cmp = (a_idx > b_idx) - (a_idx < b_idx);
        }
        if (validate_typed_array(ctx, psc->arr) < 0) {
            psc->exception = 1;
        }
    done:
        JS_FreeValue(ctx, (JSValue)argv[0]);
        JS_FreeValue(ctx, (JSValue)argv[1]);
    }
    return cmp;
}

static JSValue js_typed_array_sort(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JSObject *p;
    int len;
    size_t elt_size;
    struct TA_sort_context tsc;
    void *array_ptr;
    int (*cmpfun)(const void *a, const void *b, void *opaque);

    tsc.ctx = ctx;
    tsc.exception = 0;
    tsc.arr = this_val;
    tsc.cmp = argv[0];

    len = js_typed_array_get_length_internal(ctx, this_val);
    if (len < 0)
        return JS_EXCEPTION;
    if (!JS_IsUndefined(tsc.cmp) && check_function(ctx, tsc.cmp))
        return JS_EXCEPTION;

    if (len > 1) {
        p = JS_VALUE_GET_OBJ(this_val);
        switch (p->class_id) {
        case JS_CLASS_INT8_ARRAY:
            tsc.getfun = js_TA_get_int8;
            cmpfun = js_TA_cmp_int8;
            break;
        case JS_CLASS_UINT8C_ARRAY:
        case JS_CLASS_UINT8_ARRAY:
            tsc.getfun = js_TA_get_uint8;
            cmpfun = js_TA_cmp_uint8;
            break;
        case JS_CLASS_INT16_ARRAY:
            tsc.getfun = js_TA_get_int16;
            cmpfun = js_TA_cmp_int16;
            break;
        case JS_CLASS_UINT16_ARRAY:
            tsc.getfun = js_TA_get_uint16;
            cmpfun = js_TA_cmp_uint16;
            break;
        case JS_CLASS_INT32_ARRAY:
            tsc.getfun = js_TA_get_int32;
            cmpfun = js_TA_cmp_int32;
            break;
        case JS_CLASS_UINT32_ARRAY:
            tsc.getfun = js_TA_get_uint32;
            cmpfun = js_TA_cmp_uint32;
            break;
#ifdef CONFIG_BIGNUM
        case JS_CLASS_BIG_INT64_ARRAY:
            tsc.getfun = js_TA_get_int64;
            cmpfun = js_TA_cmp_int64;
            break;
        case JS_CLASS_BIG_UINT64_ARRAY:
            tsc.getfun = js_TA_get_uint64;
            cmpfun = js_TA_cmp_uint64;
            break;
#endif
        case JS_CLASS_FLOAT32_ARRAY:
            tsc.getfun = js_TA_get_float32;
            cmpfun = js_TA_cmp_float32;
            break;
        case JS_CLASS_FLOAT64_ARRAY:
            tsc.getfun = js_TA_get_float64;
            cmpfun = js_TA_cmp_float64;
            break;
        default:
            abort();
        }
        array_ptr = p->u.array.u.ptr;
        elt_size = 1 << typed_array_size_log2(p->class_id);
        if (!JS_IsUndefined(tsc.cmp)) {
            uint32_t *array_idx;
            void *array_tmp;
            size_t i, j;
            
            /* XXX: a stable sort would use less memory */
            array_idx = js_malloc(ctx, len * sizeof(array_idx[0]));
            if (!array_idx)
                return JS_EXCEPTION;
            for(i = 0; i < len; i++)
                array_idx[i] = i;
            tsc.array_ptr = array_ptr;
            tsc.elt_size = elt_size;
            rqsort(array_idx, len, sizeof(array_idx[0]),
                   js_TA_cmp_generic, &tsc);
            if (tsc.exception)
                goto fail;
            array_tmp = js_malloc(ctx, len * elt_size);
            if (!array_tmp) {
            fail:
                js_free(ctx, array_idx);
                return JS_EXCEPTION;
            }
            memcpy(array_tmp, array_ptr, len * elt_size);
            switch(elt_size) {
            case 1:
                for(i = 0; i < len; i++) {
                    j = array_idx[i];
                    ((uint8_t *)array_ptr)[i] = ((uint8_t *)array_tmp)[j];
                }
                break;
            case 2:
                for(i = 0; i < len; i++) {
                    j = array_idx[i];
                    ((uint16_t *)array_ptr)[i] = ((uint16_t *)array_tmp)[j];
                }
                break;
            case 4:
                for(i = 0; i < len; i++) {
                    j = array_idx[i];
                    ((uint32_t *)array_ptr)[i] = ((uint32_t *)array_tmp)[j];
                }
                break;
            case 8:
                for(i = 0; i < len; i++) {
                    j = array_idx[i];
                    ((uint64_t *)array_ptr)[i] = ((uint64_t *)array_tmp)[j];
                }
                break;
            default:
                abort();
            }
            js_free(ctx, array_tmp);
            js_free(ctx, array_idx);
        } else {
            rqsort(array_ptr, len, elt_size, cmpfun, &tsc);
            if (tsc.exception)
                return JS_EXCEPTION;
        }
    }
    return JS_DupValue(ctx, this_val);
}

static const JSCFunctionListEntry js_typed_array_base_funcs[] = {
    JS_CFUNC_DEF("from", 1, js_typed_array_from ),
    JS_CFUNC_DEF("of", 0, js_typed_array_of ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
    //JS_CFUNC_DEF("__getLength", 2, js_typed_array___getLength ),
    //JS_CFUNC_DEF("__create", 2, js_typed_array___create ),
    //JS_CFUNC_DEF("__speciesCreate", 2, js_typed_array___speciesCreate ),
};

static const JSCFunctionListEntry js_typed_array_base_proto_funcs[] = {
    JS_CGETSET_DEF("length", js_typed_array_get_length, NULL ),
    JS_CGETSET_MAGIC_DEF("buffer", js_typed_array_get_buffer, NULL, 0 ),
    JS_CGETSET_MAGIC_DEF("byteLength", js_typed_array_get_byteLength, NULL, 0 ),
    JS_CGETSET_MAGIC_DEF("byteOffset", js_typed_array_get_byteOffset, NULL, 0 ),
    JS_CFUNC_DEF("set", 1, js_typed_array_set ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_VALUE ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_KEY ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_typed_array_iterator, JS_ITERATOR_KIND_KEY_AND_VALUE ),
    JS_CGETSET_DEF("[Symbol.toStringTag]", js_typed_array_get_toStringTag, NULL ),
    JS_CFUNC_DEF("copyWithin", 2, js_typed_array_copyWithin ),
    JS_CFUNC_MAGIC_DEF("every", 1, js_array_every, special_every | special_TA ),
    JS_CFUNC_MAGIC_DEF("some", 1, js_array_every, special_some | special_TA ),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_array_every, special_forEach | special_TA ),
    JS_CFUNC_MAGIC_DEF("map", 1, js_array_every, special_map | special_TA ),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_array_every, special_filter | special_TA ),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, special_reduce | special_TA ),
    JS_CFUNC_MAGIC_DEF("reduceRight", 1, js_array_reduce, special_reduceRight | special_TA ),
    JS_CFUNC_DEF("fill", 1, js_typed_array_fill ),
    JS_CFUNC_MAGIC_DEF("find", 1, js_typed_array_find, 0 ),
    JS_CFUNC_MAGIC_DEF("findIndex", 1, js_typed_array_find, 1 ),
    JS_CFUNC_DEF("reverse", 0, js_typed_array_reverse ),
    JS_CFUNC_DEF("slice", 2, js_typed_array_slice ),
    JS_CFUNC_DEF("subarray", 2, js_typed_array_subarray ),
    JS_CFUNC_DEF("sort", 1, js_typed_array_sort ),
    JS_CFUNC_MAGIC_DEF("join", 1, js_typed_array_join, 0 ),
    JS_CFUNC_MAGIC_DEF("toLocaleString", 0, js_typed_array_join, 1 ),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_typed_array_indexOf, special_indexOf ),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_typed_array_indexOf, special_lastIndexOf ),
    JS_CFUNC_MAGIC_DEF("includes", 1, js_typed_array_indexOf, special_includes ),
    //JS_ALIAS_BASE_DEF("toString", "toString", 2 /* Array.prototype. */), @@@
};

static JSValue js_typed_array_base_constructor(JSContext *ctx,
                                               JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    return JS_ThrowTypeError(ctx, "cannot be called");
}

/* 'obj' must be an allocated typed array object */
static int typed_array_init(JSContext *ctx, JSValueConst obj,
                            JSValue buffer, uint64_t offset, uint64_t len)
{
    JSTypedArray *ta;
    JSObject *p, *pbuffer;
    JSArrayBuffer *abuf;
    int size_log2;

    p = JS_VALUE_GET_OBJ(obj);
    size_log2 = typed_array_size_log2(p->class_id);
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta) {
        JS_FreeValue(ctx, buffer);
        return -1;
    }
    pbuffer = JS_VALUE_GET_OBJ(buffer);
    abuf = pbuffer->u.array_buffer;
    ta->obj = p;
    ta->buffer = pbuffer;
    ta->offset = offset;
    ta->length = len << size_log2;
    list_add_tail(&ta->link, &abuf->array_list);
    p->u.typed_array = ta;
    p->u.array.count = len;
    p->u.array.u.ptr = abuf->data + offset;
    return 0;
}


static JSValue js_array_from_iterator(JSContext *ctx, uint32_t *plen,
                                      JSValueConst obj, JSValueConst method)
{
    JSValue arr, iter, next_method = JS_UNDEFINED, val;
    BOOL done;
    uint32_t k;

    *plen = 0;
    arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    iter = JS_GetIterator2(ctx, obj, method);
    if (JS_IsException(iter))
        goto fail;
    next_method = JS_GetProperty(ctx, iter, JS_ATOM_next);
    if (JS_IsException(next_method))
        goto fail;
    k = 0;
    for(;;) {
        val = JS_IteratorNext(ctx, iter, next_method, 0, NULL, &done);
        if (JS_IsException(val))
            goto fail;
        if (done) {
            JS_FreeValue(ctx, val);
            break;
        }
        if (JS_CreateDataPropertyUint32(ctx, arr, k, val, JS_PROP_THROW) < 0)
            goto fail;
        k++;
    }
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    *plen = k;
    return arr;
 fail:
    JS_FreeValue(ctx, next_method);
    JS_FreeValue(ctx, iter);
    JS_FreeValue(ctx, arr);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_constructor_obj(JSContext *ctx,
                                              JSValueConst new_target,
                                              JSValueConst obj,
                                              int classid)
{
    JSValue iter, ret, arr = JS_UNDEFINED, val, buffer;
    uint32_t i;
    int size_log2;
    int64_t len;

    size_log2 = typed_array_size_log2(classid);
    ret = js_create_from_ctor(ctx, new_target, classid);
    if (JS_IsException(ret))
        return JS_EXCEPTION;

    iter = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_iterator);
    if (JS_IsException(iter))
        goto fail;
    if (!JS_IsUndefined(iter) && !JS_IsNull(iter)) {
        uint32_t len1;
        arr = js_array_from_iterator(ctx, &len1, obj, iter);
        JS_FreeValue(ctx, iter);
        if (JS_IsException(arr))
            goto fail;
        len = len1;
    } else {
        if (js_get_length64(ctx, &len, obj))
            goto fail;
        arr = JS_DupValue(ctx, obj);
    }

    buffer = js_array_buffer_constructor1(ctx, JS_UNDEFINED,
                                          len << size_log2);
    if (JS_IsException(buffer))
        goto fail;
    if (typed_array_init(ctx, ret, buffer, 0, len))
        goto fail;

    for(i = 0; i < len; i++) {
        val = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsException(val))
            goto fail;
        if (JS_SetPropertyUint32(ctx, ret, i, val) < 0)
            goto fail;
    }
    JS_FreeValue(ctx, arr);
    return ret;
 fail:
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, ret);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_constructor_ta(JSContext *ctx,
                                             JSValueConst new_target,
                                             JSValueConst src_obj,
                                             int classid)
{
    JSObject *p, *src_buffer;
    JSTypedArray *ta;
    JSValue ctor, obj, buffer;
    uint32_t len, i;
    int size_log2;
    JSArrayBuffer *src_abuf, *abuf;

    obj = js_create_from_ctor(ctx, new_target, classid);
    if (JS_IsException(obj))
        return obj;
    p = JS_VALUE_GET_OBJ(src_obj);
    if (typed_array_is_detached(ctx, p)) {
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    ta = p->u.typed_array;
    len = p->u.array.count;
    src_buffer = ta->buffer;
    src_abuf = src_buffer->u.array_buffer;
    if (!src_abuf->shared) {
        ctor = JS_SpeciesConstructor(ctx, JS_MKPTR(JS_TAG_OBJECT, src_buffer),
                                     JS_UNDEFINED);
        if (JS_IsException(ctor))
            goto fail;
    } else {
        /* force ArrayBuffer default constructor */
        ctor = JS_UNDEFINED;
    }
    size_log2 = typed_array_size_log2(classid);
    buffer = js_array_buffer_constructor1(ctx, ctor,
                                          (uint64_t)len << size_log2);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(buffer))
        goto fail;
    /* necessary because it could have been detached */
    if (typed_array_is_detached(ctx, p)) {
        JS_FreeValue(ctx, buffer);
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    abuf = JS_GetOpaque(buffer, JS_CLASS_ARRAY_BUFFER);
    if (typed_array_init(ctx, obj, buffer, 0, len))
        goto fail;
    if (p->class_id == classid) {
        /* same type: copy the content */
        memcpy(abuf->data, src_abuf->data + ta->offset, abuf->byte_length);
    } else {
        for(i = 0; i < len; i++) {
            JSValue val;
            val = JS_GetPropertyUint32(ctx, src_obj, i);
            if (JS_IsException(val))
                goto fail;
            if (JS_SetPropertyUint32(ctx, obj, i, val) < 0)
                goto fail;
        }
    }
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_typed_array_constructor(JSContext *ctx,
                                          JSValueConst new_target,
                                          int argc, JSValueConst *argv,
                                          int classid)
{
    JSValue buffer, obj;
    JSArrayBuffer *abuf;
    int size_log2;
    uint64_t len, offset;

    size_log2 = typed_array_size_log2(classid);
    if (JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT) {
        if (JS_ToIndex(ctx, &len, argv[0]))
            return JS_EXCEPTION;
        buffer = js_array_buffer_constructor1(ctx, JS_UNDEFINED,
                                              len << size_log2);
        if (JS_IsException(buffer))
            return JS_EXCEPTION;
        offset = 0;
    } else {
        JSObject *p = JS_VALUE_GET_OBJ(argv[0]);
        if (p->class_id == JS_CLASS_ARRAY_BUFFER ||
            p->class_id == JS_CLASS_SHARED_ARRAY_BUFFER) {
            abuf = p->u.array_buffer;
            if (JS_ToIndex(ctx, &offset, argv[1]))
                return JS_EXCEPTION;
            if (abuf->detached)
                return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
            if ((offset & ((1 << size_log2) - 1)) != 0 ||
                offset > abuf->byte_length)
                return JS_ThrowRangeError(ctx, "invalid offset");
            if (JS_IsUndefined(argv[2])) {
                if ((abuf->byte_length & ((1 << size_log2) - 1)) != 0)
                    goto invalid_length;
                len = (abuf->byte_length - offset) >> size_log2;
            } else {
                if (JS_ToIndex(ctx, &len, argv[2]))
                    return JS_EXCEPTION;
                if (abuf->detached)
                    return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
                if ((offset + (len << size_log2)) > abuf->byte_length) {
                invalid_length:
                    return JS_ThrowRangeError(ctx, "invalid length");
                }
            }
            buffer = JS_DupValue(ctx, argv[0]);
        } else {
            if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
                p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
                return js_typed_array_constructor_ta(ctx, new_target, argv[0], classid);
            } else {
                return js_typed_array_constructor_obj(ctx, new_target, argv[0], classid);
            }
        }
    }

    obj = js_create_from_ctor(ctx, new_target, classid);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, buffer);
        return JS_EXCEPTION;
    }
    if (typed_array_init(ctx, obj, buffer, offset, len)) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static void js_typed_array_finalizer(JSRuntime *rt, JSValue val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSTypedArray *ta = p->u.typed_array;
    if (ta) {
        /* during the GC the finalizers are called in an arbitrary
           order so the ArrayBuffer finalizer may have been called */
        if (JS_IsLiveObject(rt, JS_MKPTR(JS_TAG_OBJECT, ta->buffer))) {
            list_del(&ta->link);
        }
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ta->buffer));
        js_free_rt(rt, ta);
    }
}

static void js_typed_array_mark(JSRuntime *rt, JSValueConst val,
                                JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSTypedArray *ta = p->u.typed_array;
    if (ta) {
        JS_MarkValue(rt, JS_MKPTR(JS_TAG_OBJECT, ta->buffer), mark_func);
    }
}

static JSValue js_dataview_constructor(JSContext *ctx,
                                       JSValueConst new_target,
                                       int argc, JSValueConst *argv)
{
    JSArrayBuffer *abuf;
    uint64_t offset;
    uint32_t len;
    JSValueConst buffer;
    JSValue obj;
    JSTypedArray *ta;
    JSObject *p;

    buffer = argv[0];
    abuf = js_get_array_buffer(ctx, buffer);
    if (!abuf)
        return JS_EXCEPTION;
    offset = 0;
    if (argc > 1) {
        if (JS_ToIndex(ctx, &offset, argv[1]))
            return JS_EXCEPTION;
    }
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    if (offset > abuf->byte_length)
        return JS_ThrowRangeError(ctx, "invalid byteOffset");
    len = abuf->byte_length - offset;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        uint64_t l;
        if (JS_ToIndex(ctx, &l, argv[2]))
            return JS_EXCEPTION;
        if (l > len)
            return JS_ThrowRangeError(ctx, "invalid byteLength");
        len = l;
    }

    obj = js_create_from_ctor(ctx, new_target, JS_CLASS_DATAVIEW);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (abuf->detached) {
        /* could have been detached in js_create_from_ctor() */
        JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        goto fail;
    }
    ta = js_malloc(ctx, sizeof(*ta));
    if (!ta) {
    fail:
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    p = JS_VALUE_GET_OBJ(obj);
    ta->obj = p;
    ta->buffer = JS_VALUE_GET_OBJ(JS_DupValue(ctx, buffer));
    ta->offset = offset;
    ta->length = len;
    list_add_tail(&ta->link, &abuf->array_list);
    p->u.typed_array = ta;
    return obj;
}

static JSValue js_dataview_getValue(JSContext *ctx,
                                    JSValueConst this_obj,
                                    int argc, JSValueConst *argv, int class_id)
{
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    int is_swap, size;
    uint8_t *ptr;
    uint32_t v;
    uint64_t pos;

    ta = JS_GetOpaque2(ctx, this_obj, JS_CLASS_DATAVIEW);
    if (!ta)
        return JS_EXCEPTION;
    size = 1 << typed_array_size_log2(class_id);
    if (JS_ToIndex(ctx, &pos, argv[0]))
        return JS_EXCEPTION;
    is_swap = FALSE;
    if (argc > 1)
        is_swap = JS_ToBool(ctx, argv[1]);
#ifndef WORDS_BIGENDIAN
    is_swap ^= 1;
#endif
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    if ((pos + size) > ta->length)
        return JS_ThrowRangeError(ctx, "out of bound");
    ptr = abuf->data + ta->offset + pos;

    switch(class_id) {
    case JS_CLASS_INT8_ARRAY:
        return JS_NewInt32(ctx, *(int8_t *)ptr);
    case JS_CLASS_UINT8_ARRAY:
        return JS_NewInt32(ctx, *(uint8_t *)ptr);
    case JS_CLASS_INT16_ARRAY:
        v = get_u16(ptr);
        if (is_swap)
            v = bswap16(v);
        return JS_NewInt32(ctx, (int16_t)v);
    case JS_CLASS_UINT16_ARRAY:
        v = get_u16(ptr);
        if (is_swap)
            v = bswap16(v);
        return JS_NewInt32(ctx, v);
    case JS_CLASS_INT32_ARRAY:
        v = get_u32(ptr);
        if (is_swap)
            v = bswap32(v);
        return JS_NewInt32(ctx, v);
    case JS_CLASS_UINT32_ARRAY:
        v = get_u32(ptr);
        if (is_swap)
            v = bswap32(v);
        return JS_NewUint32(ctx, v);
#ifdef CONFIG_BIGNUM
    case JS_CLASS_BIG_INT64_ARRAY:
        {
            uint64_t v;
            v = get_u64(ptr);
            if (is_swap)
                v = bswap64(v);
            return JS_NewBigInt64(ctx, v);
        }
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        {
            uint64_t v;
            v = get_u64(ptr);
            if (is_swap)
                v = bswap64(v);
            return JS_NewBigUint64(ctx, v);
        }
        break;
#endif
    case JS_CLASS_FLOAT32_ARRAY:
        {
            union {
                float f;
                uint32_t i;
            } u;
            v = get_u32(ptr);
            if (is_swap)
                v = bswap32(v);
            u.i = v;
            return __JS_NewFloat64(ctx, u.f);
        }
    case JS_CLASS_FLOAT64_ARRAY:
        {
            union {
                double f;
                uint64_t i;
            } u;
            u.i = get_u64(ptr);
            if (is_swap)
                u.i = bswap64(u.i);
            return __JS_NewFloat64(ctx, u.f);
        }
    default:
        abort();
    }
}

static JSValue js_dataview_setValue(JSContext *ctx,
                                    JSValueConst this_obj,
                                    int argc, JSValueConst *argv, int class_id)
{
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    int is_swap, size;
    uint8_t *ptr;
    uint64_t v64;
    uint32_t v;
    uint64_t pos;
    JSValueConst val;

    ta = JS_GetOpaque2(ctx, this_obj, JS_CLASS_DATAVIEW);
    if (!ta)
        return JS_EXCEPTION;
    size = 1 << typed_array_size_log2(class_id);
    if (JS_ToIndex(ctx, &pos, argv[0]))
        return JS_EXCEPTION;
    val = argv[1];
    v = 0; /* avoid warning */
    v64 = 0; /* avoid warning */
    if (class_id <= JS_CLASS_UINT32_ARRAY) {
        if (JS_ToUint32(ctx, &v, val))
            return JS_EXCEPTION;
    } else
#ifdef CONFIG_BIGNUM
    if (class_id <= JS_CLASS_BIG_UINT64_ARRAY) {
        if (JS_ToBigInt64(ctx, (int64_t *)&v64, val))
            return JS_EXCEPTION;
    } else
#endif
    {
        double d;
        if (JS_ToFloat64(ctx, &d, val))
            return JS_EXCEPTION;
        if (class_id == JS_CLASS_FLOAT32_ARRAY) {
            union {
                float f;
                uint32_t i;
            } u;
            u.f = d;
            v = u.i;
        } else {
            JSFloat64Union u;
            u.d = d;
            v64 = u.u64;
        }
    }
    is_swap = FALSE;
    if (argc > 2)
        is_swap = JS_ToBool(ctx, argv[2]);
#ifndef WORDS_BIGENDIAN
    is_swap ^= 1;
#endif
    abuf = ta->buffer->u.array_buffer;
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
    if ((pos + size) > ta->length)
        return JS_ThrowRangeError(ctx, "out of bound");
    ptr = abuf->data + ta->offset + pos;

    switch(class_id) {
    case JS_CLASS_INT8_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
        *ptr = v;
        break;
    case JS_CLASS_INT16_ARRAY:
    case JS_CLASS_UINT16_ARRAY:
        if (is_swap)
            v = bswap16(v);
        put_u16(ptr, v);
        break;
    case JS_CLASS_INT32_ARRAY:
    case JS_CLASS_UINT32_ARRAY:
    case JS_CLASS_FLOAT32_ARRAY:
        if (is_swap)
            v = bswap32(v);
        put_u32(ptr, v);
        break;
#ifdef CONFIG_BIGNUM
    case JS_CLASS_BIG_INT64_ARRAY:
    case JS_CLASS_BIG_UINT64_ARRAY:
#endif
    case JS_CLASS_FLOAT64_ARRAY:
        if (is_swap)
            v64 = bswap64(v64);
        put_u64(ptr, v64);
        break;
    default:
        abort();
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_dataview_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("buffer", js_typed_array_get_buffer, NULL, 1 ),
    JS_CGETSET_MAGIC_DEF("byteLength", js_typed_array_get_byteLength, NULL, 1 ),
    JS_CGETSET_MAGIC_DEF("byteOffset", js_typed_array_get_byteOffset, NULL, 1 ),
    JS_CFUNC_MAGIC_DEF("getInt8", 1, js_dataview_getValue, JS_CLASS_INT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getUint8", 1, js_dataview_getValue, JS_CLASS_UINT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getInt16", 1, js_dataview_getValue, JS_CLASS_INT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getUint16", 1, js_dataview_getValue, JS_CLASS_UINT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getInt32", 1, js_dataview_getValue, JS_CLASS_INT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getUint32", 1, js_dataview_getValue, JS_CLASS_UINT32_ARRAY ),
#ifdef CONFIG_BIGNUM
    JS_CFUNC_MAGIC_DEF("getBigInt64", 1, js_dataview_getValue, JS_CLASS_BIG_INT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getBigUint64", 1, js_dataview_getValue, JS_CLASS_BIG_UINT64_ARRAY ),
#endif
    JS_CFUNC_MAGIC_DEF("getFloat32", 1, js_dataview_getValue, JS_CLASS_FLOAT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("getFloat64", 1, js_dataview_getValue, JS_CLASS_FLOAT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setInt8", 2, js_dataview_setValue, JS_CLASS_INT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setUint8", 2, js_dataview_setValue, JS_CLASS_UINT8_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setInt16", 2, js_dataview_setValue, JS_CLASS_INT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setUint16", 2, js_dataview_setValue, JS_CLASS_UINT16_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setInt32", 2, js_dataview_setValue, JS_CLASS_INT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setUint32", 2, js_dataview_setValue, JS_CLASS_UINT32_ARRAY ),
#ifdef CONFIG_BIGNUM
    JS_CFUNC_MAGIC_DEF("setBigInt64", 2, js_dataview_setValue, JS_CLASS_BIG_INT64_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setBigUint64", 2, js_dataview_setValue, JS_CLASS_BIG_UINT64_ARRAY ),
#endif
    JS_CFUNC_MAGIC_DEF("setFloat32", 2, js_dataview_setValue, JS_CLASS_FLOAT32_ARRAY ),
    JS_CFUNC_MAGIC_DEF("setFloat64", 2, js_dataview_setValue, JS_CLASS_FLOAT64_ARRAY ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DataView", JS_PROP_CONFIGURABLE ),
};

/* Atomics */
#ifdef CONFIG_ATOMICS

typedef enum AtomicsOpEnum {
    ATOMICS_OP_ADD,
    ATOMICS_OP_AND,
    ATOMICS_OP_OR,
    ATOMICS_OP_SUB,
    ATOMICS_OP_XOR,
    ATOMICS_OP_EXCHANGE,
    ATOMICS_OP_COMPARE_EXCHANGE,
    ATOMICS_OP_LOAD,
} AtomicsOpEnum;

static void *js_atomics_get_ptr(JSContext *ctx,
                                JSArrayBuffer **pabuf,
                                int *psize_log2, JSClassID *pclass_id,
                                JSValueConst obj, JSValueConst idx_val,
                                int is_waitable)
{
    JSObject *p;
    JSTypedArray *ta;
    JSArrayBuffer *abuf;
    void *ptr;
    uint64_t idx;
    BOOL err;
    int size_log2;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        goto fail;
    p = JS_VALUE_GET_OBJ(obj);
#ifdef CONFIG_BIGNUM
    if (is_waitable)
        err = (p->class_id != JS_CLASS_INT32_ARRAY &&
               p->class_id != JS_CLASS_BIG_INT64_ARRAY);
    else
        err = !(p->class_id >= JS_CLASS_INT8_ARRAY &&
                p->class_id <= JS_CLASS_BIG_UINT64_ARRAY);
#else
    if (is_waitable)
        err = (p->class_id != JS_CLASS_INT32_ARRAY);
    else
        err = !(p->class_id >= JS_CLASS_INT8_ARRAY &&
                p->class_id <= JS_CLASS_UINT32_ARRAY);
#endif
    if (err) {
    fail:
        JS_ThrowTypeError(ctx, "integer TypedArray expected");
        return NULL;
    }
    ta = p->u.typed_array;
    abuf = ta->buffer->u.array_buffer;
    if (!abuf->shared) {
        if (is_waitable == 2) {
            JS_ThrowTypeError(ctx, "not a SharedArrayBuffer TypedArray");
            return NULL;
        }
        if (abuf->detached) {
            JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
            return NULL;
        }
    }
    if (JS_ToIndex(ctx, &idx, idx_val)) {
        return NULL;
    }
    /* if the array buffer is detached, p->u.array.count = 0 */
    if (idx >= p->u.array.count) {
        JS_ThrowRangeError(ctx, "out-of-bound access");
        return NULL;
    }
    size_log2 = typed_array_size_log2(p->class_id);
    ptr = p->u.array.u.uint8_ptr + ((uintptr_t)idx << size_log2);
    if (pabuf)
        *pabuf = abuf;
    if (psize_log2)
        *psize_log2 = size_log2;
    if (pclass_id)
        *pclass_id = p->class_id;
    return ptr;
}

static JSValue js_atomics_op(JSContext *ctx,
                             JSValueConst this_obj,
                             int argc, JSValueConst *argv, int op)
{
    int size_log2;
#ifdef CONFIG_BIGNUM
    uint64_t v, a, rep_val;
#else
    uint32_t v, a, rep_val;
#endif
    void *ptr;
    JSValue ret;
    JSClassID class_id;
    JSArrayBuffer *abuf;

    ptr = js_atomics_get_ptr(ctx, &abuf, &size_log2, &class_id,
                             argv[0], argv[1], 0);
    if (!ptr)
        return JS_EXCEPTION;
    rep_val = 0;
    if (op == ATOMICS_OP_LOAD) {
        v = 0;
    } else {
#ifdef CONFIG_BIGNUM
        if (size_log2 == 3) {
            int64_t v64;
            if (JS_ToBigInt64(ctx, &v64, argv[2]))
                return JS_EXCEPTION;
            v = v64;
            if (op == ATOMICS_OP_COMPARE_EXCHANGE) {
                if (JS_ToBigInt64(ctx, &v64, argv[3]))
                    return JS_EXCEPTION;
                rep_val = v64;
            }
        } else
#endif
        {
                uint32_t v32;
                if (JS_ToUint32(ctx, &v32, argv[2]))
                    return JS_EXCEPTION;
                v = v32;
                if (op == ATOMICS_OP_COMPARE_EXCHANGE) {
                    if (JS_ToUint32(ctx, &v32, argv[3]))
                        return JS_EXCEPTION;
                    rep_val = v32;
                }
        }
        if (abuf->detached)
            return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
   }

   switch(op | (size_log2 << 3)) {
            
#ifdef CONFIG_BIGNUM
#define OP(op_name, func_name)                          \
    case ATOMICS_OP_ ## op_name | (0 << 3):             \
       a = func_name((_Atomic(uint8_t) *)ptr, v);       \
       break;                                           \
    case ATOMICS_OP_ ## op_name | (1 << 3):             \
        a = func_name((_Atomic(uint16_t) *)ptr, v);     \
        break;                                          \
    case ATOMICS_OP_ ## op_name | (2 << 3):             \
        a = func_name((_Atomic(uint32_t) *)ptr, v);     \
        break;                                          \
    case ATOMICS_OP_ ## op_name | (3 << 3):             \
        a = func_name((_Atomic(uint64_t) *)ptr, v);     \
        break;
#else
#define OP(op_name, func_name)                          \
    case ATOMICS_OP_ ## op_name | (0 << 3):             \
       a = func_name((_Atomic(uint8_t) *)ptr, v);       \
       break;                                           \
    case ATOMICS_OP_ ## op_name | (1 << 3):             \
        a = func_name((_Atomic(uint16_t) *)ptr, v);     \
        break;                                          \
    case ATOMICS_OP_ ## op_name | (2 << 3):             \
        a = func_name((_Atomic(uint32_t) *)ptr, v);     \
        break;
#endif
        OP(ADD, atomic_fetch_add)
        OP(AND, atomic_fetch_and)
        OP(OR, atomic_fetch_or)
        OP(SUB, atomic_fetch_sub)
        OP(XOR, atomic_fetch_xor)
        OP(EXCHANGE, atomic_exchange)
#undef OP

    case ATOMICS_OP_LOAD | (0 << 3):
        a = atomic_load((_Atomic(uint8_t) *)ptr);
        break;
    case ATOMICS_OP_LOAD | (1 << 3):
        a = atomic_load((_Atomic(uint16_t) *)ptr);
        break;
    case ATOMICS_OP_LOAD | (2 << 3):
        a = atomic_load((_Atomic(uint32_t) *)ptr);
        break;
#ifdef CONFIG_BIGNUM
    case ATOMICS_OP_LOAD | (3 << 3):
        a = atomic_load((_Atomic(uint64_t) *)ptr);
        break;
#endif
        
    case ATOMICS_OP_COMPARE_EXCHANGE | (0 << 3):
        {
            uint8_t v1 = v;
            atomic_compare_exchange_strong((_Atomic(uint8_t) *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
    case ATOMICS_OP_COMPARE_EXCHANGE | (1 << 3):
        {
            uint16_t v1 = v;
            atomic_compare_exchange_strong((_Atomic(uint16_t) *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
    case ATOMICS_OP_COMPARE_EXCHANGE | (2 << 3):
        {
            uint32_t v1 = v;
            atomic_compare_exchange_strong((_Atomic(uint32_t) *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
#ifdef CONFIG_BIGNUM
    case ATOMICS_OP_COMPARE_EXCHANGE | (3 << 3):
        {
            uint64_t v1 = v;
            atomic_compare_exchange_strong((_Atomic(uint64_t) *)ptr, &v1, rep_val);
            a = v1;
        }
        break;
#endif
    default:
        abort();
    }

    switch(class_id) {
    case JS_CLASS_INT8_ARRAY:
        a = (int8_t)a;
        goto done;
    case JS_CLASS_UINT8_ARRAY:
        a = (uint8_t)a;
        goto done;
    case JS_CLASS_INT16_ARRAY:
        a = (int16_t)a;
        goto done;
    case JS_CLASS_UINT16_ARRAY:
        a = (uint16_t)a;
        goto done;
    case JS_CLASS_INT32_ARRAY:
    done:
        ret = JS_NewInt32(ctx, a);
        break;
    case JS_CLASS_UINT32_ARRAY:
        ret = JS_NewUint32(ctx, a);
        break;
#ifdef CONFIG_BIGNUM
    case JS_CLASS_BIG_INT64_ARRAY:
        ret = JS_NewBigInt64(ctx, a);
        break;
    case JS_CLASS_BIG_UINT64_ARRAY:
        ret = JS_NewBigUint64(ctx, a);
        break;
#endif
    default:
        abort();
    }
    return ret;
}

static JSValue js_atomics_store(JSContext *ctx,
                                JSValueConst this_obj,
                                int argc, JSValueConst *argv)
{
    int size_log2;
    void *ptr;
    JSValue ret;
    JSArrayBuffer *abuf;

    ptr = js_atomics_get_ptr(ctx, &abuf, &size_log2, NULL,
                             argv[0], argv[1], 0);
    if (!ptr)
        return JS_EXCEPTION;
#ifdef CONFIG_BIGNUM
    if (size_log2 == 3) {
        int64_t v64;
        ret = JS_ToBigIntValueFree(ctx, JS_DupValue(ctx, argv[2]));
        if (JS_IsException(ret))
            return ret;
        if (JS_ToBigInt64(ctx, &v64, ret)) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        if (abuf->detached)
            return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        atomic_store((_Atomic(uint64_t) *)ptr, v64);
    } else
#endif
    {
        uint32_t v;
        /* XXX: spec, would be simpler to return the written value */
        ret = JS_ToIntegerFree(ctx, JS_DupValue(ctx, argv[2]));
        if (JS_IsException(ret))
            return ret;
        if (JS_ToUint32(ctx, &v, ret)) {
            JS_FreeValue(ctx, ret);
            return JS_EXCEPTION;
        }
        if (abuf->detached)
            return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);
        switch(size_log2) {
        case 0:
            atomic_store((_Atomic(uint8_t) *)ptr, v);
            break;
        case 1:
            atomic_store((_Atomic(uint16_t) *)ptr, v);
            break;
        case 2:
            atomic_store((_Atomic(uint32_t) *)ptr, v);
            break;
        default:
            abort();
        }
    }
    return ret;
}

static JSValue js_atomics_isLockFree(JSContext *ctx,
                                     JSValueConst this_obj,
                                     int argc, JSValueConst *argv)
{
    int v, ret;
    if (JS_ToInt32Sat(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    ret = (v == 1 || v == 2 || v == 4
#ifdef CONFIG_BIGNUM
           || v == 8
#endif
           );
    return JS_NewBool(ctx, ret);
}

typedef struct JSAtomicsWaiter {
    struct list_head link;
    BOOL linked;
    pthread_cond_t cond;
    int32_t *ptr;
} JSAtomicsWaiter;

static pthread_mutex_t js_atomics_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct list_head js_atomics_waiter_list =
    LIST_HEAD_INIT(js_atomics_waiter_list);

static JSValue js_atomics_wait(JSContext *ctx,
                               JSValueConst this_obj,
                               int argc, JSValueConst *argv)
{
    int64_t v;
    int32_t v32;
    void *ptr;
    int64_t timeout;
    struct timespec ts;
    JSAtomicsWaiter waiter_s, *waiter;
    int ret, size_log2, res;
    double d;

    ptr = js_atomics_get_ptr(ctx, NULL, &size_log2, NULL,
                             argv[0], argv[1], 2);
    if (!ptr)
        return JS_EXCEPTION;
#ifdef CONFIG_BIGNUM
    if (size_log2 == 3) {
        if (JS_ToBigInt64(ctx, &v, argv[2]))
            return JS_EXCEPTION;
    } else
#endif
    {        
        if (JS_ToInt32(ctx, &v32, argv[2]))
            return JS_EXCEPTION;
        v = v32;
    }
    if (JS_ToFloat64(ctx, &d, argv[3]))
        return JS_EXCEPTION;
    if (isnan(d) || d > INT64_MAX)
        timeout = INT64_MAX;
    else if (d < 0)
        timeout = 0;
    else
        timeout = (int64_t)d;
    if (!ctx->rt->can_block)
        return JS_ThrowTypeError(ctx, "cannot block in this thread");

    /* XXX: inefficient if large number of waiters, should hash on
       'ptr' value */
    /* XXX: use Linux futexes when available ? */
    pthread_mutex_lock(&js_atomics_mutex);
    if (size_log2 == 3) {
        res = *(int64_t *)ptr != v;
    } else {
        res = *(int32_t *)ptr != v;
    }
    if (res) {
        pthread_mutex_unlock(&js_atomics_mutex);
        return JS_AtomToString(ctx, JS_ATOM_not_equal);
    }

    waiter = &waiter_s;
    waiter->ptr = ptr;
    pthread_cond_init(&waiter->cond, NULL);
    waiter->linked = TRUE;
    list_add_tail(&waiter->link, &js_atomics_waiter_list);

    if (timeout == INT64_MAX) {
        pthread_cond_wait(&waiter->cond, &js_atomics_mutex);
        ret = 0;
    } else {
        /* XXX: use clock monotonic */
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout / 1000;
        ts.tv_nsec += (timeout % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_nsec -= 1000000000;
            ts.tv_sec++;
        }
        ret = pthread_cond_timedwait(&waiter->cond, &js_atomics_mutex,
                                     &ts);
    }
    if (waiter->linked)
        list_del(&waiter->link);
    pthread_mutex_unlock(&js_atomics_mutex);
    pthread_cond_destroy(&waiter->cond);
    if (ret == ETIMEDOUT) {
        return JS_AtomToString(ctx, JS_ATOM_timed_out);
    } else {
        return JS_AtomToString(ctx, JS_ATOM_ok);
    }
}

static JSValue js_atomics_notify(JSContext *ctx,
                                 JSValueConst this_obj,
                                 int argc, JSValueConst *argv)
{
    struct list_head *el, *el1, waiter_list;
    int32_t count, n;
    void *ptr;
    JSAtomicsWaiter *waiter;
    JSArrayBuffer *abuf;

    ptr = js_atomics_get_ptr(ctx, &abuf, NULL, NULL, argv[0], argv[1], 1);
    if (!ptr)
        return JS_EXCEPTION;

    if (JS_IsUndefined(argv[2])) {
        count = INT32_MAX;
    } else {
        if (JS_ToInt32Clamp(ctx, &count, argv[2], 0, INT32_MAX, 0))
            return JS_EXCEPTION;
    }
    if (abuf->detached)
        return JS_ThrowTypeErrorDetachedArrayBuffer(ctx);

    n = 0;
    if (abuf->shared && count > 0) {
        pthread_mutex_lock(&js_atomics_mutex);
        init_list_head(&waiter_list);
        list_for_each_safe(el, el1, &js_atomics_waiter_list) {
            waiter = list_entry(el, JSAtomicsWaiter, link);
            if (waiter->ptr == ptr) {
                list_del(&waiter->link);
                waiter->linked = FALSE;
                list_add_tail(&waiter->link, &waiter_list);
                n++;
                if (n >= count)
                    break;
            }
        }
        list_for_each(el, &waiter_list) {
            waiter = list_entry(el, JSAtomicsWaiter, link);
            pthread_cond_signal(&waiter->cond);
        }
        pthread_mutex_unlock(&js_atomics_mutex);
    }
    return JS_NewInt32(ctx, n);
}

static const JSCFunctionListEntry js_atomics_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 3, js_atomics_op, ATOMICS_OP_ADD ),
    JS_CFUNC_MAGIC_DEF("and", 3, js_atomics_op, ATOMICS_OP_AND ),
    JS_CFUNC_MAGIC_DEF("or", 3, js_atomics_op, ATOMICS_OP_OR ),
    JS_CFUNC_MAGIC_DEF("sub", 3, js_atomics_op, ATOMICS_OP_SUB ),
    JS_CFUNC_MAGIC_DEF("xor", 3, js_atomics_op, ATOMICS_OP_XOR ),
    JS_CFUNC_MAGIC_DEF("exchange", 3, js_atomics_op, ATOMICS_OP_EXCHANGE ),
    JS_CFUNC_MAGIC_DEF("compareExchange", 4, js_atomics_op, ATOMICS_OP_COMPARE_EXCHANGE ),
    JS_CFUNC_MAGIC_DEF("load", 2, js_atomics_op, ATOMICS_OP_LOAD ),
    JS_CFUNC_DEF("store", 3, js_atomics_store ),
    JS_CFUNC_DEF("isLockFree", 1, js_atomics_isLockFree ),
    JS_CFUNC_DEF("wait", 4, js_atomics_wait ),
    JS_CFUNC_DEF("notify", 3, js_atomics_notify ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Atomics", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_atomics_obj[] = {
    JS_OBJECT_DEF("Atomics", js_atomics_funcs, countof(js_atomics_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

void JS_AddIntrinsicAtomics(JSContext *ctx)
{
    /* add Atomics as autoinit object */
    JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_atomics_obj, countof(js_atomics_obj));
}

#endif /* CONFIG_ATOMICS */

void JS_AddIntrinsicTypedArrays(JSContext *ctx)
{
    JSValue typed_array_base_proto, typed_array_base_func;
    JSValueConst array_buffer_func, shared_array_buffer_func;
    int i;

    ctx->class_proto[JS_CLASS_ARRAY_BUFFER] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_ARRAY_BUFFER],
                               js_array_buffer_proto_funcs,
                               countof(js_array_buffer_proto_funcs));

    array_buffer_func = JS_NewGlobalCConstructorOnly(ctx, "ArrayBuffer",
                                                 js_array_buffer_constructor, 1,
                                                 ctx->class_proto[JS_CLASS_ARRAY_BUFFER]);
    JS_SetPropertyFunctionList(ctx, array_buffer_func,
                               js_array_buffer_funcs,
                               countof(js_array_buffer_funcs));

    ctx->class_proto[JS_CLASS_SHARED_ARRAY_BUFFER] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_SHARED_ARRAY_BUFFER],
                               js_shared_array_buffer_proto_funcs,
                               countof(js_shared_array_buffer_proto_funcs));

    shared_array_buffer_func = JS_NewGlobalCConstructorOnly(ctx, "SharedArrayBuffer",
                                                 js_shared_array_buffer_constructor, 1,
                                                 ctx->class_proto[JS_CLASS_SHARED_ARRAY_BUFFER]);
    JS_SetPropertyFunctionList(ctx, shared_array_buffer_func,
                               js_shared_array_buffer_funcs,
                               countof(js_shared_array_buffer_funcs));

    typed_array_base_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, typed_array_base_proto,
                               js_typed_array_base_proto_funcs,
                               countof(js_typed_array_base_proto_funcs));

    /* TypedArray.prototype.toString must be the same object as Array.prototype.toString */
    JSValue obj = JS_GetProperty(ctx, ctx->class_proto[JS_CLASS_ARRAY], JS_ATOM_toString);
    /* XXX: should use alias method in JSCFunctionListEntry */ //@@@
    JS_DefinePropertyValue(ctx, typed_array_base_proto, JS_ATOM_toString, obj,
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

    typed_array_base_func = JS_NewCFunction(ctx, js_typed_array_base_constructor,
                                            "TypedArray", 0);
    JS_SetPropertyFunctionList(ctx, typed_array_base_func,
                               js_typed_array_base_funcs,
                               countof(js_typed_array_base_funcs));
    JS_SetConstructor(ctx, typed_array_base_func, typed_array_base_proto);

    for(i = JS_CLASS_UINT8C_ARRAY; i < JS_CLASS_UINT8C_ARRAY + JS_TYPED_ARRAY_COUNT; i++) {
        JSValue func_obj;
        char buf[ATOM_GET_STR_BUF_SIZE];
        const char *name;

        ctx->class_proto[i] = JS_NewObjectProto(ctx, typed_array_base_proto);
        JS_DefinePropertyValueStr(ctx, ctx->class_proto[i],
                                  "BYTES_PER_ELEMENT",
                                  JS_NewInt32(ctx, 1 << typed_array_size_log2(i)),
                                  0);
        name = JS_AtomGetStr(ctx, buf, sizeof(buf),
                             JS_ATOM_Uint8ClampedArray + i - JS_CLASS_UINT8C_ARRAY);
        func_obj = JS_NewCFunction3(ctx, (JSCFunction *)js_typed_array_constructor,
                                    name, 3, JS_CFUNC_constructor_magic, i,
                                    typed_array_base_func);
        JS_NewGlobalCConstructor2(ctx, func_obj, name, ctx->class_proto[i]);
        JS_DefinePropertyValueStr(ctx, func_obj,
                                  "BYTES_PER_ELEMENT",
                                  JS_NewInt32(ctx, 1 << typed_array_size_log2(i)),
                                  0);
    }
    JS_FreeValue(ctx, typed_array_base_proto);
    JS_FreeValue(ctx, typed_array_base_func);

    /* DataView */
    ctx->class_proto[JS_CLASS_DATAVIEW] = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_DATAVIEW],
                               js_dataview_proto_funcs,
                               countof(js_dataview_proto_funcs));
    JS_NewGlobalCConstructorOnly(ctx, "DataView",
                                 js_dataview_constructor, 1,
                                 ctx->class_proto[JS_CLASS_DATAVIEW]);
    /* Atomics */
#ifdef CONFIG_ATOMICS
    JS_AddIntrinsicAtomics(ctx);
#endif
}
