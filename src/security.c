
/* 
 * Lamb Gateway Platform
 * Copyright (C) 2017 typefo <typefo@qq.com>
 */

#include <stdio.h>
#include "security.h"

bool lamb_security_check(lamb_cache_t *cache, int type, char *phone) {
    char cmd[128];
    bool r = false;
    redisReply *reply = NULL;
    
    snprintf(cmd, sizeof(cmd), "EXISTS %s%s", (type == LAMB_BLACKLIST) ? "black." : "white.", phone);
    reply = redisCommand(cache->handle, cmd);
    if (reply != NULL) {
        if (reply->type == REDIS_REPLY_INTEGER) {
            r = (reply->integer == 1) ? true : false;
        }
        freeReplyObject(reply);
    }

    return r;
}
