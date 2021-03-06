
/* 
 * Lamb Gateway Platform
 * Copyright (C) 2017 typefo <typefo@qq.com>
 */

#ifndef _LAMB_GATEWAY_H
#define _LAMB_GATEWAY_H

#include "db.h"
#include "cache.h"

typedef struct {
    int id;
    int type;
    char name[12];
    char host[32];
    int port;
    char username[32];
    char password[64];
    char spid[8];
    char spcode[21];
    int encoding;
    bool extended;
    int concurrent;
} lamb_gateway_t;

int lamb_get_gateway(lamb_db_t *db, int id, lamb_gateway_t *gateway);
int lamb_get_gateways(lamb_db_t *db, lamb_gateway_t **gateways, size_t size);

#endif
