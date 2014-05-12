
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "net.h"
#include "rules.h"

#define HASH_LENGTH 40
#define MAX_BUCKET_LENGTH 256
#define HASH_MASK 0xFF

typedef char functionHash[HASH_LENGTH + 1];

typedef struct mapEntry {
    unsigned int sidHash;
    unsigned int bindingIndex;
} mapEntry;

typedef struct binding {
    redisContext *reContext;
    functionHash dequeueActionHash;
    functionHash partitionHash;
    char *actionSortedset;
    char *messageHashset;
    char *sessionHashset;
    char *partitionHashset;
    functionHash *hashArray;
    unsigned int hashArrayLength;
} binding;

typedef struct bindingsMap {
    mapEntry entries[MAX_BUCKET_LENGTH];
    binding *bindings;
    unsigned int bindingsLength;
    unsigned int lastBinding;
} bindingsMap;

unsigned int djbHash(char *str, unsigned int len) {
   unsigned int hash = 5381;
   unsigned int i = 0;

   for(i = 0; i < len; str++, i++) {
      hash = ((hash << 5) + hash) + (*str);
   }

   return hash;
}

static unsigned int loadCommands(ruleset *tree, binding *rulesBinding) {
    redisContext *reContext = rulesBinding->reContext;
    redisReply *reply;
    rulesBinding->hashArray = malloc(tree->actionCount * sizeof(functionHash));
    for (unsigned int i = 0; i < tree->nodeOffset; ++i) {
        char *lua = NULL;
        char *oldLua;
        node *currentNode = &tree->nodePool[i];
        if (currentNode->type == NODE_ACTION) {
            unsigned int queryLength = currentNode->value.c.queryLength;
            for (unsigned int ii = 0; ii < queryLength; ++ii) {
                unsigned int lineOffset = tree->queryPool[currentNode->value.c.queryOffset + ii];
                char *currentLine = &tree->stringPool[lineOffset];
                char *last = currentLine;
                unsigned int clauseCount = 0;
                while (last[0] != '\0') {
                    if (last[0] == ' ') {
                        last[0] = '\0';
                        if (lua) {
                            oldLua = lua;
                            asprintf(&lua, "%skey = \"%s!\" .. ARGV[2]\n"
                                           "res = redis.call(\"zrange\", key, 0, 0)\n"
                                           "if (res[1]) then\n"
                                           "signature = signature .. \",\" .. key .. \",\" .. res[1]\n", 
                                    lua, currentLine);
                            free(oldLua);
                        } else {
                            asprintf(&lua, "key = \"%s!\" .. ARGV[2]\n"
                                           "res = redis.call(\"zrange\", key, 0, 0)\n"
                                           "if (res[1]) then\n"
                                           "signature = signature .. \",\" .. key .. \",\" .. res[1]\n", 
                                    currentLine);
                        }

                        last[0] = ' ';
                        currentLine = last + 1;
                        ++clauseCount;
                    }
                    ++last;
                } 

                oldLua = lua;
                asprintf(&lua, "%sredis.call(\"zadd\", KEYS[2], ARGV[5], signature)\n", lua);
                free(oldLua);

                for (unsigned int iii = 0; iii < clauseCount; ++iii) {
                    oldLua = lua;
                    asprintf(&lua, "%send\n", lua);
                    free(oldLua);
                }
            }

            oldLua = lua;
            asprintf(&lua, "local res\n"
                           "local signature = \"$s\" .. \",\" .. ARGV[2]\n"
                           "local key = ARGV[1] .. \"!\" .. ARGV[2]\n"
                           "local result = 0\n"
                           "if (ARGV[6] == \"1\") then\n"
                           "  redis.call(\"hset\", KEYS[1], ARGV[2], ARGV[4])\n"
                           "  redis.call(\"zadd\", key, ARGV[5], ARGV[2])\n"
                           "else\n"
                           "  result = redis.call(\"hsetnx\", KEYS[3], ARGV[2], \"{ \\\"id\\\":\\\"\" .. ARGV[2] .. \"\\\" }\")\n"
                           "  redis.call(\"hsetnx\", KEYS[1], ARGV[3], ARGV[4])\n"
                           "  redis.call(\"zadd\", key, ARGV[5], ARGV[3])\n"
                           "end\n%sreturn result\n", oldLua);
                           
                           
            redisAppendCommand(reContext, "SCRIPT LOAD %s", lua);
            redisGetReply(reContext, (void**)&reply);
            if (reply->type == REDIS_REPLY_ERROR) {
                freeReplyObject(reply);
                free(lua);
                return ERR_REDIS_ERROR;
            }

            functionHash *currentAssertHash = &rulesBinding->hashArray[currentNode->value.c.index];
            strncpy(*currentAssertHash, reply->str, HASH_LENGTH);
            (*currentAssertHash)[HASH_LENGTH] = '\0';
            freeReplyObject(reply);
            free(lua);
            free(oldLua);
        }
    }

    redisAppendCommand(reContext, "SCRIPT LOAD %s", 
                    "local signature = redis.call(\"zrange\", KEYS[3], 0, 0, \"withscores\")\n"
                    "local timestamp = tonumber(ARGV[1])\n"
                    "if (signature[2] ~= nil and (tonumber(signature[2]) < (timestamp + 100))) then\n"
                    "  redis.call(\"zincrby\", KEYS[3], 60000, signature[1])\n"
                    "  local i = 0\n"
                    "  local res = { signature[1] }\n"
                    "  local skip = 0\n"
                    "  for token in string.gmatch(signature[1], \"([%w%.%-%$_!]+)\") do\n"
                    "    if (i > 1 and string.match(token, \"%$s!\")) then\n"
                    "      table.insert(res, token)\n"
                    "      skip = 1\n"
                    "    elseif (skip == 1) then\n"
                    "      table.insert(res, \"null\")\n"
                    "      skip = 0\n"
                    "    elseif (i % 2 == 0) then\n"
                    "      table.insert(res, token)\n"
                    "    else\n"
                    "      if (i == 1) then\n"
                    "        table.insert(res, redis.call(\"hget\", KEYS[1], token))\n"
                    "      else\n"
                    "        table.insert(res, redis.call(\"hget\", KEYS[2], token))\n"
                    "      end\n"
                    "    end\n"
                    "    i = i + 1\n"
                    "  end\n"
                    "  return res\n"
                    "end\n"
                    "return false\n");

    redisGetReply(reContext, (void**)&reply);
    if (reply->type == REDIS_REPLY_ERROR) {
        freeReplyObject(reply);
        return ERR_REDIS_ERROR;
    }

    strncpy(rulesBinding->dequeueActionHash, reply->str, 40);
    rulesBinding->dequeueActionHash[40] = '\0';
    freeReplyObject(reply);

    redisAppendCommand(reContext, "SCRIPT LOAD %s", 
                    "local res = redis.call(\"hget\", KEYS[1], ARGV[1])\n"
                    "if (not res) then\n"
                    "   res = redis.call(\"hincrby\", KEYS[1], \"index\", 1)\n"
                    "   res = res % tonumber(ARGV[2])\n"
                    "   redis.call(\"hset\", KEYS[1], ARGV[1], res)\n"
                    "end\n"
                    "return tonumber(res)\n");

    redisGetReply(reContext, (void**)&reply);
    if (reply->type == REDIS_REPLY_ERROR) {
        freeReplyObject(reply);
        return ERR_REDIS_ERROR;
    }

    strncpy(rulesBinding->partitionHash, reply->str, 40);
    rulesBinding->partitionHash[40] = '\0';
    freeReplyObject(reply);

    char *name = &tree->stringPool[tree->nameOffset];
    int nameLength = strlen(name);
    char *sessionHashset = malloc((nameLength + 3) * sizeof(char));
    if (!sessionHashset) {
        return ERR_OUT_OF_MEMORY;
    }

    strncpy(sessionHashset, name, nameLength);
    sessionHashset[nameLength] = '!';
    sessionHashset[nameLength + 1] = 's';
    sessionHashset[nameLength + 2] = '\0';
    rulesBinding->sessionHashset = sessionHashset;

    char *messageHashset = malloc((nameLength + 3) * sizeof(char));
    if (!messageHashset) {
        return ERR_OUT_OF_MEMORY;
    }

    strncpy(messageHashset, name, nameLength);
    messageHashset[nameLength] = '!';
    messageHashset[nameLength + 1] = 'm';
    messageHashset[nameLength + 2] = '\0';
    rulesBinding->messageHashset = messageHashset;

    char *actionSortedset = malloc((nameLength + 3) * sizeof(char));
    if (!actionSortedset) {
        return ERR_OUT_OF_MEMORY;
    }

    strncpy(actionSortedset, name, nameLength);
    actionSortedset[nameLength] = '!';
    actionSortedset[nameLength + 1] = 'a';
    actionSortedset[nameLength + 2] = '\0';
    rulesBinding->actionSortedset = actionSortedset;

    char *partitionHashset = malloc((nameLength + 3) * sizeof(char));
    if (!partitionHashset) {
        return ERR_OUT_OF_MEMORY;
    }

    strncpy(partitionHashset, name, nameLength);
    partitionHashset[nameLength] = '!';
    partitionHashset[nameLength + 1] = 'p';
    partitionHashset[nameLength + 2] = '\0';
    rulesBinding->partitionHashset = partitionHashset;
    return RULES_OK;
}

unsigned int bindRuleset(void *handle, char *path) {
    ruleset *tree = (ruleset*)handle;
    bindingsMap *map;
    if (tree->bindingsMap) {
        map = tree->bindingsMap;
    }
    else {
        map = malloc(sizeof(bindingsMap));
        if (!map) {
            return ERR_OUT_OF_MEMORY;
        }

        memset(map->entries, 0, sizeof(mapEntry) * MAX_BUCKET_LENGTH);
        map->bindings = NULL;
        map->bindingsLength = 0;
        map->lastBinding = 0;
        tree->bindingsMap = map;
    }

    redisContext *reContext = redisConnectUnix(path);
    //redisContext *reContext = redisConnect((char*)"127.0.0.1", atol(path));
    if (reContext->err) {
        redisFree(reContext);
        return ERR_CONNECT_REDIS;
    }

    if (!map->bindings) {
        map->bindings = malloc(sizeof(binding));
    }
    else {
        map->bindings = realloc(map->bindings, sizeof(binding) * (map->bindingsLength + 1));
    }

    if (!map->bindings) {
        redisFree(reContext);
        return ERR_OUT_OF_MEMORY;
    }
    map->bindings[map->bindingsLength].reContext = reContext;
    ++map->bindingsLength;
    return loadCommands(tree, &map->bindings[map->bindingsLength -1]);
}

unsigned int deleteBindingsMap(ruleset *tree) {
    bindingsMap *map = tree->bindingsMap;
    for (unsigned int i = 0; i < map->bindingsLength; ++i) {
        binding *currentBinding = &map->bindings[i];
        redisFree(currentBinding->reContext);
        free(currentBinding->actionSortedset);
        free(currentBinding->messageHashset);
        free(currentBinding->sessionHashset);
        free(currentBinding->partitionHashset);
        free(currentBinding->hashArray);
    }

    free(map->bindings);
    free(map);
    return RULES_OK;
}

static unsigned int resolveBinding(ruleset *tree, char *sid, binding **rulesBinding) {
    bindingsMap *map = tree->bindingsMap;
    unsigned int sidHash = djbHash(sid, strlen(sid));
    mapEntry *entry = &map->entries[sidHash & HASH_MASK];
    if (entry->sidHash != sidHash) {
        binding *firstBinding = &map->bindings[0];
        redisContext *reContext = firstBinding->reContext;
        int result = redisAppendCommand(reContext, "EVALSHA %s %d %s %d %d", firstBinding->partitionHash, 1, 
                                        firstBinding->partitionHashset, sidHash, map->bindingsLength);
        if (result != REDIS_OK) {
            return ERR_REDIS_ERROR;
        }

        redisReply *reply;
        result = redisGetReply(reContext, (void**)&reply);
        if (result != REDIS_OK) {
            return ERR_REDIS_ERROR;
        }
        
        if (reply->type == REDIS_REPLY_ERROR) {
            freeReplyObject(reply);
            return ERR_REDIS_ERROR;
        } 

        entry->sidHash = sidHash;
        entry->bindingIndex = reply->integer;
    }

    *rulesBinding = &map->bindings[entry->bindingIndex];
    return RULES_OK;
}

unsigned int assertMessageImmediate(ruleset *tree, void **bindingContext, char *key, char *sid, char *mid, char *message, unsigned int actionIndex) {
    int result;
    if (*bindingContext == NULL) {
        result = resolveBinding(tree, sid, (binding**)bindingContext);
        if (result != RULES_OK) {
            return result;
        }
    }

    binding *rulesBinding = (binding*)*bindingContext;
    redisContext *reContext = rulesBinding->reContext;
    time_t currentTime = time(NULL);
    functionHash *currentAssertHash = &rulesBinding->hashArray[actionIndex];
    result = redisAppendCommand(reContext, "EVALSHA %s %d %s %s %s %s %s %s %s %ld 0", *currentAssertHash, 3, rulesBinding->messageHashset, 
                            rulesBinding->actionSortedset, rulesBinding->sessionHashset, key, sid, mid, message, currentTime); 
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }

    redisReply *reply;
    result = redisGetReply(reContext, (void**)&reply);
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }

    if (reply->type == REDIS_REPLY_ERROR) {
        freeReplyObject(reply);
        return ERR_REDIS_ERROR;
    }

    if (reply->type == REDIS_REPLY_INTEGER && reply->integer) {
        freeReplyObject(reply);
        return ERR_NEW_SESSION;
    }
    
    freeReplyObject(reply);    
    return RULES_OK;
}

unsigned int peekAction(ruleset *tree, void **rulesBinding, redisReply **reply) {
    bindingsMap *map = tree->bindingsMap;
    for (unsigned int i = 0; i < map->bindingsLength; ++i) {
        binding *currentBinding = &map->bindings[map->lastBinding % map->bindingsLength];
        ++map->lastBinding;
        redisContext *reContext = currentBinding->reContext;
        time_t currentTime = time(NULL);
        int result = redisAppendCommand(reContext, "EVALSHA %s %d %s %s %s %ld", currentBinding->dequeueActionHash, 3, 
                           currentBinding->sessionHashset, currentBinding->messageHashset, currentBinding->actionSortedset, currentTime); 
        if (result != REDIS_OK) {
            continue;
        }

        result = redisGetReply(reContext, (void**)reply);
        if (result != REDIS_OK) {
            freeReplyObject(*reply);
            continue;
        }
        
        if ((*reply)->type == REDIS_REPLY_ARRAY) {
            *rulesBinding = currentBinding;
            return RULES_OK;
        } else {
            freeReplyObject(*reply);
        }
    }

    return ERR_NO_ACTION_AVAILABLE;
}

unsigned int negateMessage(void *rulesBinding, char *key, char *sid, char *mid) {
    redisContext *reContext = ((binding*)rulesBinding)->reContext;
    int result = redisAppendCommand(reContext, "ZREM %s!%s %s", key, sid, mid);
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }

    return RULES_OK;
}

unsigned int assertSession(void *rulesBinding, char *key, char *sid, char *state, unsigned int actionIndex) {
    binding *currentBinding = (binding*)rulesBinding;
    redisContext *reContext = currentBinding->reContext;
    time_t currentTime = time(NULL);
    functionHash *currentAssertHash = &currentBinding->hashArray[actionIndex];
    int result = redisAppendCommand(reContext, "EVALSHA %s %d %s %s %s %s %s %s %ld 1", *currentAssertHash, 2, 
                       currentBinding->sessionHashset, currentBinding->actionSortedset, key, sid, sid, state, currentTime); 
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }

    return RULES_OK;
}

unsigned int assertSessionImmediate(void *rulesBinding, char *key, char *sid, char *state, unsigned int actionIndex) {
    int result = assertSession(rulesBinding, key, sid, state, actionIndex);
    if (result != RULES_OK) {
        return result;
    }

    binding *currentBinding = (binding*)rulesBinding;
    redisContext *reContext = currentBinding->reContext;
    redisReply *reply;
    result = redisGetReply(reContext, (void**)&reply);
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }

    if (reply->type == REDIS_REPLY_ERROR) {
        freeReplyObject(reply);
        return ERR_REDIS_ERROR;
    }

    freeReplyObject(reply);    
    return RULES_OK;
}

unsigned int negateSession(void *rulesBinding, char *key, char *sid) {
    redisContext *reContext = ((binding*)rulesBinding)->reContext;
    int result = redisAppendCommand(reContext, "ZREM %s!%s %s", key, sid, sid);
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }
    
    return RULES_OK;
}

unsigned int removeAction(void *rulesBinding, char *action) {
    binding *currentBinding = (binding*)rulesBinding;
    redisContext *reContext = currentBinding->reContext;   
    int result = redisAppendCommand(reContext, "ZREM %s %s", currentBinding->actionSortedset, action);
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }
    
    return RULES_OK;
}

unsigned int removeMessage(void *rulesBinding, char *mid) {
    binding *currentBinding = (binding*)rulesBinding;
    redisContext *reContext = currentBinding->reContext;  
    int result = redisAppendCommand(reContext, "HDEL %s %s", currentBinding->messageHashset, mid);
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }
    
    return RULES_OK;
}

unsigned int prepareCommands(void *rulesBinding) {
    redisContext *reContext = ((binding*)rulesBinding)->reContext;   
    int result = redisAppendCommand(reContext, "MULTI");
    if (result != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }
    
    return RULES_OK;
}

unsigned int executeCommands(void *rulesBinding, unsigned short commandCount) {
    redisContext *reContext = ((binding*)rulesBinding)->reContext;  
    int redisResult = redisAppendCommand(reContext, "EXEC");
    if (redisResult != REDIS_OK) {
        return ERR_REDIS_ERROR;
    }

    redisReply *reply;
    unsigned int result = RULES_OK;
    for (unsigned short i = 0; i < commandCount + 2; ++i) {
        redisResult = redisGetReply(reContext, (void**)&reply);
        if (redisResult != REDIS_OK) {
            result = ERR_REDIS_ERROR;
        } else {
            if (reply->type == REDIS_REPLY_ERROR) {
                result = ERR_REDIS_ERROR;
            }

            freeReplyObject(reply);    
        } 
    }
    
    return result;
}
