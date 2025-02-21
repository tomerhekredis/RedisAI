#pragma once

#include "config/config.h"
#include "tensor_struct.h"

typedef struct RAI_Script {
    void *script;
    char *scriptdef;
    // TODO: scripts do not have placement in PyTorch
    // Placement depends on the inputs, as do outputs
    // We keep it here at the moment, until we have a
    // CUDA allocator for dlpack
    char *devicestr;
    RedisModuleString *tag;
    long long refCount;
    void *infokey;
} RAI_Script;

typedef struct RAI_ScriptCtxParam {
    RAI_Tensor *tensor;
} RAI_ScriptCtxParam;

typedef struct RAI_ScriptRunCtx {
    size_t ctxtype;
    RAI_Script *script;
    char *fnname;
    RAI_ScriptCtxParam *inputs;
    RAI_ScriptCtxParam *outputs;
    int variadic;
} RAI_ScriptRunCtx;
