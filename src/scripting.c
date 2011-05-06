#include "redis.h"
#include "sha1.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

char *redisProtocolToLuaType_Int(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Bulk(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Status(lua_State *lua, char *reply);
char *redisProtocolToLuaType_Error(lua_State *lua, char *reply);
char *redisProtocolToLuaType_MultiBulk(lua_State *lua, char *reply);

/* Take a Redis reply in the Redis protocol format and convert it into a
 * Lua type. Thanks to this function, and the introduction of not connected
 * clients, it is trvial to implement the redis() lua function.
 *
 * Basically we take the arguments, execute the Redis command in the context
 * of a non connected client, then take the generated reply and convert it
 * into a suitable Lua type. With this trick the scripting feature does not
 * need the introduction of a full Redis internals API. Basically the script
 * is like a normal client that bypasses all the slow I/O paths.
 *
 * Note: in this function we do not do any sanity check as the reply is
 * generated by Redis directly. This allows use to go faster.
 * The reply string can be altered during the parsing as it is discared
 * after the conversion is completed.
 *
 * Errors are returned as a table with a single 'err' field set to the
 * error string.
 */

char *redisProtocolToLuaType(lua_State *lua, char* reply) {
    char *p = reply;

    switch(*p) {
    case ':':
        p = redisProtocolToLuaType_Int(lua,reply);
        break;
    case '$':
        p = redisProtocolToLuaType_Bulk(lua,reply);
        break;
    case '+':
        p = redisProtocolToLuaType_Status(lua,reply);
        break;
    case '-':
        p = redisProtocolToLuaType_Error(lua,reply);
        break;
    case '*':
        p = redisProtocolToLuaType_MultiBulk(lua,reply);
        break;
    }
    return p;
}

char *redisProtocolToLuaType_Int(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long value;

    string2ll(reply+1,p-reply-1,&value);
    lua_pushnumber(lua,(lua_Number)value);
    return p+2;
}

char *redisProtocolToLuaType_Bulk(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long bulklen;

    string2ll(reply+1,p-reply-1,&bulklen);
    if (bulklen == -1) {
        lua_pushnil(lua);
        return p+2;
    } else {
        lua_pushlstring(lua,p+2,bulklen);
        return p+2+bulklen+2;
    }
}

char *redisProtocolToLuaType_Status(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');

    lua_newtable(lua);
    lua_pushstring(lua,"ok");
    lua_pushlstring(lua,reply+1,p-reply-1);
    lua_settable(lua,-3);
    return p+2;
}

char *redisProtocolToLuaType_Error(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');

    lua_newtable(lua);
    lua_pushstring(lua,"err");
    lua_pushlstring(lua,reply+1,p-reply-1);
    lua_settable(lua,-3);
    return p+2;
}

char *redisProtocolToLuaType_MultiBulk(lua_State *lua, char *reply) {
    char *p = strchr(reply+1,'\r');
    long long mbulklen;
    int j = 0;

    string2ll(reply+1,p-reply-1,&mbulklen);
    p += 2;
    if (mbulklen == -1) {
        lua_pushnil(lua);
        return p;
    }
    lua_newtable(lua);
    for (j = 0; j < mbulklen; j++) {
        lua_pushnumber(lua,j+1);
        p = redisProtocolToLuaType(lua,p);
        lua_settable(lua,-3);
    }
    return p;
}

void luaPushError(lua_State *lua, char *error) {
    lua_newtable(lua);
    lua_pushstring(lua,"err");
    lua_pushstring(lua, error);
    lua_settable(lua,-3);
}

int luaRedisCommand(lua_State *lua) {
    int j, argc = lua_gettop(lua);
    struct redisCommand *cmd;
    robj **argv;
    redisClient *c = server.lua_client;
    sds reply;

    /* Build the arguments vector */
    argv = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        if (!lua_isstring(lua,j+1)) break;
        argv[j] = createStringObject((char*)lua_tostring(lua,j+1),
                                     lua_strlen(lua,j+1));
    }
    
    /* Check if one of the arguments passed by the Lua script
     * is not a string or an integer (lua_isstring() return true for
     * integers as well). */
    if (j != argc) {
        j--;
        while (j >= 0) {
            decrRefCount(argv[j]);
            j--;
        }
        zfree(argv);
        luaPushError(lua,
            "Lua redis() command arguments must be strings or integers");
        return 1;
    }

    /* Command lookup */
    cmd = lookupCommand(argv[0]->ptr);
    if (!cmd || ((cmd->arity > 0 && cmd->arity != argc) ||
                   (argc < -cmd->arity)))
    {
        for (j = 0; j < argc; j++) decrRefCount(argv[j]);
        zfree(argv);
        if (cmd)
            luaPushError(lua,
                "Wrong number of args calling Redis command From Lua script");
        else
            luaPushError(lua,"Unknown Redis command called from Lua script");
        return 1;
    }

    /* Run the command in the context of a fake client */
    c->argv = argv;
    c->argc = argc;
    cmd->proc(c);

    /* Convert the result of the Redis command into a suitable Lua type.
     * The first thing we need is to create a single string from the client
     * output buffers. */
    reply = sdsempty();
    if (c->bufpos) {
        reply = sdscatlen(reply,c->buf,c->bufpos);
        c->bufpos = 0;
    }
    while(listLength(c->reply)) {
        robj *o = listNodeValue(listFirst(c->reply));

        reply = sdscatlen(reply,o->ptr,sdslen(o->ptr));
        listDelNode(c->reply,listFirst(c->reply));
    }
    redisProtocolToLuaType(lua,reply);
    sdsfree(reply);

    /* Clean up. Command code may have changed argv/argc so we use the
     * argv/argc of the client instead of the local variables. */
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);

    return 1;
}

void luaMaskCountHook(lua_State *lua, lua_Debug *ar) {
    long long elapsed;
    REDIS_NOTUSED(ar);

    if (server.lua_time_limit <= 0) return;
    elapsed = (ustime()/1000) - server.lua_time_start;
    if (elapsed >= server.lua_time_limit) {
        lua_pushstring(lua,"Script aborted for max execution time...");
        lua_error(lua);
        redisLog(REDIS_NOTICE,"Lua script aborted for max execution time after %lld milliseconds of running time",elapsed);
    }
}

void scriptingInit(void) {
    lua_State *lua = lua_open();
    luaL_openlibs(lua);

    /* Register the 'r' command */
    lua_pushcfunction(lua,luaRedisCommand);
    lua_setglobal(lua,"redis");

    /* Create the (non connected) client that we use to execute Redis commands
     * inside the Lua interpreter */
    server.lua_client = createClient(-1);
    server.lua_client->flags |= REDIS_LUA_CLIENT;

    /* Set an hook in order to be able to stop the script execution if it
     * is running for too much time. */
    lua_sethook(lua,luaMaskCountHook,LUA_MASKCOUNT,100000);

    server.lua = lua;
}

/* Hash the scripit into a SHA1 digest. We use this as Lua function name.
 * Digest should point to a 41 bytes buffer: 40 for SHA1 converted into an
 * hexadecimal number, plus 1 byte for null term. */
void hashScript(char *digest, char *script, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char *cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx,(unsigned char*)script,len);
    SHA1Final(hash,&ctx);

    for (j = 0; j < 20; j++) {
        digest[j*2] = cset[((hash[j]&0xF0)>>4)];
        digest[j*2+1] = cset[(hash[j]&0xF)];
    }
    digest[40] = '\0';
}

void luaReplyToRedisReply(redisClient *c, lua_State *lua) {
    int t = lua_type(lua,1);

    switch(t) {
    case LUA_TSTRING:
        addReplyBulkCBuffer(c,(char*)lua_tostring(lua,1),lua_strlen(lua,1));
        break;
    case LUA_TBOOLEAN:
        addReply(c,lua_toboolean(lua,1) ? shared.cone : shared.czero);
        break;
    case LUA_TNUMBER:
        addReplyLongLong(c,(long long)lua_tonumber(lua,1));
        break;
    case LUA_TTABLE:
        /* We need to check if it is an array, an error, or a status reply.
         * Error are returned as a single element table with 'err' field.
         * Status replies are returned as single elment table with 'ok' field */
        lua_pushstring(lua,"err");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            addReplySds(c,sdscatprintf(sdsempty(),
                    "-%s\r\n",(char*)lua_tostring(lua,-1)));
            lua_pop(lua,2);
            return;
        }

        lua_pop(lua,1);
        lua_pushstring(lua,"ok");
        lua_gettable(lua,-2);
        t = lua_type(lua,-1);
        if (t == LUA_TSTRING) {
            addReplySds(c,sdscatprintf(sdsempty(),
                    "+%s\r\n",(char*)lua_tostring(lua,-1)));
            lua_pop(lua,1);
        } else {
            void *replylen = addDeferredMultiBulkLength(c);
            int j = 1, mbulklen = 0;

            lua_pop(lua,1); /* Discard the 'ok' field value we popped */
            while(1) {
                lua_pushnumber(lua,j++);
                lua_gettable(lua,-2);
                t = lua_type(lua,-1);
                if (t == LUA_TNIL) {
                    lua_pop(lua,1);
                    break;
                } else if (t == LUA_TSTRING) {
                    size_t len;
                    char *s = (char*) lua_tolstring(lua,-1,&len);

                    addReplyBulkCBuffer(c,s,len);
                    mbulklen++;
                } else if (t == LUA_TNUMBER) {
                    addReplyLongLong(c,(long long)lua_tonumber(lua,-1));
                    mbulklen++;
                }
                lua_pop(lua,1);
            }
            setDeferredMultiBulkLength(c,replylen,mbulklen);
        }
        break;
    default:
        addReply(c,shared.nullbulk);
    }
    lua_pop(lua,1);
}

/* Set an array of Redis String Objects as a Lua array (table) stored into a
 * global variable. */
void luaSetGlobalArray(lua_State *lua, char *var, robj **elev, int elec) {
    int j;

    lua_newtable(lua);
    for (j = 0; j < elec; j++) {
        lua_pushlstring(lua,(char*)elev[j]->ptr,sdslen(elev[j]->ptr));
        lua_rawseti(lua,-2,j+1);
    }
    lua_setglobal(lua,var);
}

void evalCommand(redisClient *c) {
    lua_State *lua = server.lua;
    char funcname[43];
    long long numkeys;

    /* Get the number of arguments that are keys */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&numkeys,NULL) != REDIS_OK)
        return;
    if (numkeys > (c->argc - 3)) {
        addReplyError(c,"Number of keys can't be greater than number of args");
        return;
    }

    /* We obtain the script SHA1, then check if this function is already
     * defined into the Lua state */
    funcname[0] = 'f';
    funcname[1] = '_';
    hashScript(funcname+2,c->argv[1]->ptr,sdslen(c->argv[1]->ptr));
    lua_getglobal(lua, funcname);
    if (lua_isnil(lua,1)) {
        /* Function not defined... let's define it. */
        sds funcdef = sdsempty();

        lua_pop(lua,1); /* remove the nil from the stack */
        funcdef = sdscat(funcdef,"function ");
        funcdef = sdscatlen(funcdef,funcname,42);
        funcdef = sdscatlen(funcdef," ()\n",4);
        funcdef = sdscatlen(funcdef,c->argv[1]->ptr,sdslen(c->argv[1]->ptr));
        funcdef = sdscatlen(funcdef,"\nend\n",5);
        /* printf("Defining:\n%s\n",funcdef); */

        if (luaL_loadbuffer(lua,funcdef,sdslen(funcdef),"func definition")) {
            addReplyErrorFormat(c,"Error compiling script (new function): %s\n",
                lua_tostring(lua,-1));
            lua_pop(lua,1);
            sdsfree(funcdef);
            return;
        }
        sdsfree(funcdef);
        if (lua_pcall(lua,0,0,0)) {
            addReplyErrorFormat(c,"Error running script (new function): %s\n",
                lua_tostring(lua,-1));
            lua_pop(lua,1);
            return;
        }
        lua_getglobal(lua, funcname);
    }

    /* Populate the argv and keys table accordingly to the arguments that
     * EVAL received. */
    luaSetGlobalArray(lua,"KEYS",c->argv+3,numkeys);
    luaSetGlobalArray(lua,"ARGV",c->argv+3+numkeys,c->argc-3-numkeys);

    /* Select the right DB in the context of the Lua client */
    selectDb(server.lua_client,c->db->id);
    
    /* At this point whatever this script was never seen before or if it was
     * already defined, we can call it. We have zero arguments and expect
     * a single return value. */
    server.lua_time_start = ustime()/1000;
    if (lua_pcall(lua,0,1,0)) {
        selectDb(c,server.lua_client->db->id); /* set DB ID from Lua client */
        addReplyErrorFormat(c,"Error running script (call to %s): %s\n",
            funcname, lua_tostring(lua,-1));
        lua_pop(lua,1);
        lua_gc(lua,LUA_GCCOLLECT,0);
        return;
    }
    selectDb(c,server.lua_client->db->id); /* set DB ID from Lua client */
    luaReplyToRedisReply(c,lua);
    lua_gc(lua,LUA_GCSTEP,1);
}
