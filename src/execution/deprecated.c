#include "deprecated.h"
#include "modelRun_ctx.h"
#include "command_parser.h"
#include "util/string_utils.h"
#include "execution/utils.h"
#include "rmutil/args.h"
#include "backends/backends.h"
#include "execution/background_workers.h"
#include "redis_ai_objects/stats.h"

static int _ModelRunCommand_ParseArgs(RedisModuleCtx *ctx, int argc, RedisModuleString **argv,
                                      RAI_Model **model, RAI_Error *error,
                                      RedisModuleString ***inkeys, RedisModuleString ***outkeys,
                                      RedisModuleString **runkey, long long *timeout) {

    if (argc < 6) {
        RAI_SetError(error, RAI_EMODELRUN,
                     "ERR wrong number of arguments for 'AI.MODELRUN' command");
        return REDISMODULE_ERR;
    }
    size_t argpos = 1;
    const int status = RAI_GetModelFromKeyspace(ctx, argv[argpos], model, REDISMODULE_READ, error);
    if (status == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    RAI_HoldString(NULL, argv[argpos]);
    *runkey = argv[argpos];
    const char *arg_string = RedisModule_StringPtrLen(argv[++argpos], NULL);

    // Parse timeout arg if given and store it in timeout
    if (!strcasecmp(arg_string, "TIMEOUT")) {
        if (ParseTimeout(argv[++argpos], error, timeout) == REDISMODULE_ERR)
            return REDISMODULE_ERR;
        arg_string = RedisModule_StringPtrLen(argv[++argpos], NULL);
    }
    if (strcasecmp(arg_string, "INPUTS") != 0) {
        RAI_SetError(error, RAI_EMODELRUN, "ERR INPUTS not specified");
        return REDISMODULE_ERR;
    }

    bool is_input = true, is_output = false;
    size_t ninputs = 0, noutputs = 0;

    while (++argpos < argc) {
        arg_string = RedisModule_StringPtrLen(argv[argpos], NULL);
        if (!strcasecmp(arg_string, "OUTPUTS") && !is_output) {
            is_input = false;
            is_output = true;
        } else {
            RAI_HoldString(NULL, argv[argpos]);
            if (is_input) {
                ninputs++;
                *inkeys = array_append(*inkeys, argv[argpos]);
            } else {
                noutputs++;
                *outkeys = array_append(*outkeys, argv[argpos]);
            }
        }
    }
    if ((*model)->ninputs != ninputs) {
        RAI_SetError(error, RAI_EMODELRUN,
                     "Number of keys given as INPUTS here does not match model definition");
        return REDISMODULE_ERR;
    }

    if ((*model)->noutputs != noutputs) {
        RAI_SetError(error, RAI_EMODELRUN,
                     "Number of keys given as OUTPUTS here does not match model definition");
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

int ParseModelRunCommand(RedisAI_RunInfo *rinfo, RAI_DagOp *currentOp, RedisModuleString **argv,
                         int argc) {

    int res = REDISMODULE_ERR;
    // Build a ModelRunCtx from command.
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
    RAI_Model *model;
    long long timeout = 0;
    if (_ModelRunCommand_ParseArgs(ctx, argc, argv, &model, rinfo->err, &currentOp->inkeys,
                                   &currentOp->outkeys, &currentOp->runkey,
                                   &timeout) == REDISMODULE_ERR) {
        goto cleanup;
    }

    if (timeout > 0 && !rinfo->single_op_dag) {
        RAI_SetError(rinfo->err, RAI_EDAGBUILDER, "ERR TIMEOUT not allowed within a DAG command");
        goto cleanup;
    }

    RAI_ModelRunCtx *mctx = RAI_ModelRunCtxCreate(model);
    currentOp->commandType = REDISAI_DAG_CMD_MODELRUN;
    currentOp->mctx = mctx;
    currentOp->devicestr = mctx->model->devicestr;

    if (rinfo->single_op_dag) {
        rinfo->timeout = timeout;
        // Set params in ModelRunCtx, bring inputs from key space.
        if (ModelRunCtx_SetParams(ctx, currentOp->inkeys, currentOp->outkeys, mctx, rinfo->err) ==
            REDISMODULE_ERR)
            goto cleanup;
    }
    res = REDISMODULE_OK;

cleanup:
    RedisModule_FreeThreadSafeContext(ctx);
    return res;
}

int ModelSetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4)
        return RedisModule_WrongArity(ctx);

    ArgsCursor ac;
    ArgsCursor_InitRString(&ac, argv + 1, argc - 1);

    RedisModuleString *keystr;
    AC_GetRString(&ac, &keystr, 0);

    const char *bckstr;
    int backend;
    AC_GetString(&ac, &bckstr, NULL, 0);
    if (strcasecmp(bckstr, "TF") == 0) {
        backend = RAI_BACKEND_TENSORFLOW;
    } else if (strcasecmp(bckstr, "TFLITE") == 0) {
        backend = RAI_BACKEND_TFLITE;
    } else if (strcasecmp(bckstr, "TORCH") == 0) {
        backend = RAI_BACKEND_TORCH;
    } else if (strcasecmp(bckstr, "ONNX") == 0) {
        backend = RAI_BACKEND_ONNXRUNTIME;
    } else {
        return RedisModule_ReplyWithError(ctx, "ERR unsupported backend");
    }

    const char *devicestr;
    AC_GetString(&ac, &devicestr, NULL, 0);

    if (strlen(devicestr) > 10 || strcasecmp(devicestr, "INPUTS") == 0 ||
        strcasecmp(devicestr, "OUTPUTS") == 0 || strcasecmp(devicestr, "TAG") == 0 ||
        strcasecmp(devicestr, "BATCHSIZE") == 0 || strcasecmp(devicestr, "MINBATCHSIZE") == 0 ||
        strcasecmp(devicestr, "MINBATCHTIMEOUT") == 0 || strcasecmp(devicestr, "BLOB") == 0) {
        return RedisModule_ReplyWithError(ctx, "ERR Invalid DEVICE");
    }

    RedisModuleString *tag = NULL;
    if (AC_AdvanceIfMatch(&ac, "TAG")) {
        AC_GetRString(&ac, &tag, 0);
    }

    unsigned long long batchsize = 0;
    if (AC_AdvanceIfMatch(&ac, "BATCHSIZE")) {
        if (backend == RAI_BACKEND_TFLITE) {
            return RedisModule_ReplyWithError(
                ctx, "ERR Auto-batching not supported by the TFLITE backend");
        }
        if (AC_GetUnsignedLongLong(&ac, &batchsize, 0) != AC_OK) {
            return RedisModule_ReplyWithError(ctx, "ERR Invalid argument for BATCHSIZE");
        }
    }

    unsigned long long minbatchsize = 0;
    if (AC_AdvanceIfMatch(&ac, "MINBATCHSIZE")) {
        if (batchsize == 0) {
            return RedisModule_ReplyWithError(ctx, "ERR MINBATCHSIZE specified without BATCHSIZE");
        }
        if (AC_GetUnsignedLongLong(&ac, &minbatchsize, 0) != AC_OK) {
            return RedisModule_ReplyWithError(ctx, "ERR Invalid argument for MINBATCHSIZE");
        }
    }

    unsigned long long minbatchtimeout = 0;
    if (AC_AdvanceIfMatch(&ac, "MINBATCHTIMEOUT")) {
        if (batchsize == 0) {
            return RedisModule_ReplyWithError(ctx,
                                              "ERR MINBATCHTIMEOUT specified without BATCHSIZE");
        }
        if (minbatchsize == 0) {
            return RedisModule_ReplyWithError(ctx,
                                              "ERR MINBATCHTIMEOUT specified without MINBATCHSIZE");
        }
        if (AC_GetUnsignedLongLong(&ac, &minbatchtimeout, 0) != AC_OK) {
            return RedisModule_ReplyWithError(ctx, "ERR Invalid argument for MINBATCHTIMEOUT");
        }
    }

    if (AC_IsAtEnd(&ac)) {
        return RedisModule_ReplyWithError(ctx, "ERR Insufficient arguments, missing model BLOB");
    }

    ArgsCursor optionsac;
    const char *blob_matches[] = {"BLOB"};
    AC_GetSliceUntilMatches(&ac, &optionsac, 1, blob_matches);

    if (optionsac.argc == 0 && backend == RAI_BACKEND_TENSORFLOW) {
        return RedisModule_ReplyWithError(
            ctx, "ERR Insufficient arguments, INPUTS and OUTPUTS not specified");
    }

    ArgsCursor inac = {0};
    ArgsCursor outac = {0};
    if (optionsac.argc > 0 && backend == RAI_BACKEND_TENSORFLOW) {
        if (!AC_AdvanceIfMatch(&optionsac, "INPUTS")) {
            return RedisModule_ReplyWithError(ctx, "ERR INPUTS not specified");
        }

        const char *matches[] = {"OUTPUTS"};
        AC_GetSliceUntilMatches(&optionsac, &inac, 1, matches);

        if (AC_IsAtEnd(&optionsac) || !AC_AdvanceIfMatch(&optionsac, "OUTPUTS")) {
            return RedisModule_ReplyWithError(ctx, "ERR OUTPUTS not specified");
        }
        AC_GetSliceToEnd(&optionsac, &outac);
    }

    size_t ninputs = inac.argc;
    const char *inputs[ninputs];
    for (size_t i = 0; i < ninputs; i++) {
        AC_GetString(&inac, inputs + i, NULL, 0);
    }

    size_t noutputs = outac.argc;
    const char *outputs[noutputs];
    for (size_t i = 0; i < noutputs; i++) {
        AC_GetString(&outac, outputs + i, NULL, 0);
    }

    RAI_ModelOpts opts = {
        .batchsize = batchsize,
        .minbatchsize = minbatchsize,
        .minbatchtimeout = minbatchtimeout,
        .backends_intra_op_parallelism = getBackendsIntraOpParallelism(),
        .backends_inter_op_parallelism = getBackendsInterOpParallelism(),
    };

    RAI_Model *model = NULL;

    AC_AdvanceUntilMatches(&ac, 1, blob_matches);

    if (AC_Advance(&ac) != AC_OK || AC_IsAtEnd(&ac)) {
        return RedisModule_ReplyWithError(ctx, "ERR Insufficient arguments, missing model BLOB");
    }

    ArgsCursor blobsac;
    AC_GetSliceToEnd(&ac, &blobsac);

    size_t modellen;
    char *modeldef;

    if (blobsac.argc == 1) {
        AC_GetString(&blobsac, (const char **)&modeldef, &modellen, 0);
    } else {
        const char *chunks[blobsac.argc];
        size_t chunklens[blobsac.argc];
        modellen = 0;
        while (!AC_IsAtEnd(&blobsac)) {
            AC_GetString(&blobsac, &chunks[blobsac.offset], &chunklens[blobsac.offset], 0);
            modellen += chunklens[blobsac.offset - 1];
        }

        modeldef = RedisModule_Calloc(modellen, sizeof(char));
        size_t offset = 0;
        for (size_t i = 0; i < blobsac.argc; i++) {
            memcpy(modeldef + offset, chunks[i], chunklens[i]);
            offset += chunklens[i];
        }
    }

    RAI_Error err = {0};

    model = RAI_ModelCreate(backend, devicestr, tag, opts, ninputs, inputs, noutputs, outputs,
                            modeldef, modellen, &err);

    if (err.code == RAI_EBACKENDNOTLOADED) {
        RedisModule_Log(ctx, "warning", "backend %s not loaded, will try loading default backend",
                        bckstr);
        int ret = RAI_LoadDefaultBackend(ctx, backend);
        if (ret == REDISMODULE_ERR) {
            RedisModule_Log(ctx, "error", "could not load %s default backend", bckstr);
            int ret = RedisModule_ReplyWithError(ctx, "ERR Could not load backend");
            RAI_ClearError(&err);
            return ret;
        }
        RAI_ClearError(&err);
        model = RAI_ModelCreate(backend, devicestr, tag, opts, ninputs, inputs, noutputs, outputs,
                                modeldef, modellen, &err);
    }

    if (blobsac.argc > 1) {
        RedisModule_Free(modeldef);
    }

    if (err.code != RAI_OK) {
        RedisModule_Log(ctx, "error", "%s", err.detail);
        int ret = RedisModule_ReplyWithError(ctx, err.detail_oneline);
        RAI_ClearError(&err);
        return ret;
    }

    // TODO: if backend loaded, make sure there's a queue
    RunQueueInfo *run_queue_info = NULL;
    if (ensureRunQueue(devicestr, &run_queue_info) != REDISMODULE_OK) {
        RAI_ModelFree(model, &err);
        if (err.code != RAI_OK) {
            RedisModule_Log(ctx, "error", "%s", err.detail);
            int ret = RedisModule_ReplyWithError(ctx, err.detail_oneline);
            RAI_ClearError(&err);
            return ret;
        }
        return RedisModule_ReplyWithError(ctx,
                                          "ERR Could not initialize queue on requested device");
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, keystr, REDISMODULE_READ | REDISMODULE_WRITE);
    int type = RedisModule_KeyType(key);
    if (type != REDISMODULE_KEYTYPE_EMPTY &&
        !(type == REDISMODULE_KEYTYPE_MODULE &&
          RedisModule_ModuleTypeGetType(key) == RedisAI_ModelType)) {
        RedisModule_CloseKey(key);
        RAI_ModelFree(model, &err);
        if (err.code != RAI_OK) {
            RedisModule_Log(ctx, "error", "%s", err.detail);
            int ret = RedisModule_ReplyWithError(ctx, err.detail_oneline);
            RAI_ClearError(&err);
            return ret;
        }
        return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    }

    RedisModule_ModuleTypeSetValue(key, RedisAI_ModelType, model);

    model->infokey = RAI_AddStatsEntry(ctx, keystr, RAI_MODEL, backend, devicestr, tag);

    RedisModule_CloseKey(key);

    RedisModule_ReplyWithSimpleString(ctx, "OK");

    RedisModule_ReplicateVerbatim(ctx);

    return REDISMODULE_OK;
}
