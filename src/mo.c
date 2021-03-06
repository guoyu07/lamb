
/* 
 * Lamb Gateway Platform
 * Copyright (C) 2017 typefo <typefo@qq.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <nanomsg/nn.h>
#include <nanomsg/pair.h>
#include <nanomsg/reqrep.h>
#include <syslog.h>
#include "common.h"
#include "config.h"
#include "cache.h"
#include "socket.h"
#include "message.h"
#include "log.h"
#include "mo.h"

static lamb_cache_t *rdb;
static lamb_list_t *pool;
static lamb_config_t config;
static pthread_cond_t cond;
static pthread_mutex_t mutex;
static Response resp = RESPONSE__INIT;

int main(int argc, char *argv[]) {
    char *file = "mo.conf";
    bool background = false;

    int opt = 0;
    char *optstring = "c:d";
    opt = getopt(argc, argv, optstring);

    while (opt != -1) {
        switch (opt) {
        case 'c':
            file = optarg;
            break;
        case 'd':
            background = true;
            break;
        }
        opt = getopt(argc, argv, optstring);
    }


    /* Read lamb configuration file */
    if (lamb_read_config(&config, file) != 0) {
        return -1;
    }
    
    /* Daemon mode */
    if (background) {
        lamb_daemon();
    }

    /* Logger initialization*/
    lamb_log_init("lamb-mo");

    /* Check lock protection */
    lamb_lock_t lock;

    if (lamb_lock_protection(&lock, "/tmp/mo.lock")) {
        syslog(LOG_ERR, "Already started, please do not repeat the start");
        return -1;
    }

    /* Save pid to file */
    lamb_pid_file(&lock, getpid());

    /* Signal event processing */
    lamb_signal_processing();

    /* Setting process information */
    lamb_set_process("lamb-mod");

    /* Start Main Event Thread */
    lamb_event_loop();

    /* Release lock protection */
    lamb_lock_release(&lock);

    return 0;
}

void lamb_event_loop(void) {
    int fd;
    int err;
    char addr[128];

    pthread_cond_init(&cond, NULL);
    pthread_mutex_init(&mutex, NULL);
    
    /* Client Queue Pools Initialization */
    pool = lamb_list_new();
    if (!pool) {
        syslog(LOG_ERR, "queue pool initialization failed");
        return;
    }

    pool->match = lamb_queue_compare;

    /* Redis Initialization */
    rdb = (lamb_cache_t *)malloc(sizeof(lamb_cache_t));

    if (!rdb) {
        syslog(LOG_ERR, "The kernel can't allocate memory");
        return;
    }

    err = lamb_cache_connect(rdb, config.redis_host, config.redis_port,
                             NULL, config.redis_db);

    if (err) {
        syslog(LOG_ERR, "can't connect to redis database %s", config.redis_host);
        return;
    }

    /* Server Initialization */
    fd = nn_socket(AF_SP, NN_REP);
    if (fd < 0) {
        syslog(LOG_ERR, "socket %s", nn_strerror(nn_errno()));
        return;
    }

    snprintf(addr, sizeof(addr), "tcp://%s:%d", config.listen, config.port);

    if (nn_bind(fd, addr) < 0) {
        nn_close(fd);
        syslog(LOG_ERR, "bind %s", nn_strerror(nn_errno()));
        return;
    }

    /* Start Data Acquisition Thread */
    lamb_start_thread(lamb_stat_loop, NULL, 1);

    int rc, len;
    
    while (true) {
        char *buf = NULL;
        rc = nn_recv(fd, &buf, NN_MSG, 0);

        if (rc < HEAD) {
            if (rc > 0) {
                nn_freemsg(buf);
            }
            continue;
        }

        if (CHECK_COMMAND(buf) != LAMB_REQUEST) {
            nn_freemsg(buf);
            syslog(LOG_ERR, "Invalid command request from client");
            continue;
        }

        Request *req;
        req = request__unpack(NULL, rc - HEAD, (uint8_t *)(buf + HEAD));
        nn_freemsg(buf);

        if (!req) {
            syslog(LOG_ERR, "can't parse protobuff protocol packets");
            continue;
        }

        if (req->id < 1) {
            request__free_unpacked(req, NULL);
            syslog(LOG_ERR, "Invalid client identity id number");
            continue;
        }

        struct timeval now;
        struct timespec timeout;
        
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += 3;

        pthread_mutex_lock(&mutex);

        if (req->type == LAMB_PULL) {
            lamb_start_thread(lamb_pull_loop,  (void *)req, 1);
        } else if (req->type == LAMB_PUSH) {
            lamb_start_thread(lamb_push_loop,  (void *)req, 1);
        } else {
            pthread_mutex_unlock(&mutex);
            continue;
        }

        err = pthread_cond_timedwait(&cond, &mutex, &timeout);

        if (err != ETIMEDOUT) {
            void *pk;
            len = response__get_packed_size(&resp);
            pk = malloc(len);

            if (pk) {
                response__pack(&resp, pk);
                len = lamb_pack_assembly(&buf, LAMB_RESPONSE, pk, len);
                if (len > 0) {
                    nn_send(fd, buf, len, 0);
                    free(buf);
                }
                free(pk);
            }
        }

        pthread_mutex_unlock(&mutex);
    }
}

void *lamb_push_loop(void *arg) {
    int fd;
    int err;
    int timeout;
    char host[64];
    Request *client;
    lamb_node_t *node;
    lamb_queue_t *queue;
    
    client = (Request *)arg;

    syslog(LOG_INFO, "new client from %s connectd\n", client->addr);

    /* Client queue initialization */
    node = lamb_list_find(pool, (void *)(intptr_t)client->id);

    if (node) {
        queue = (lamb_queue_t *)node->val;
    } else {
        queue = lamb_queue_new(client->id);
        if (queue) {
            lamb_list_rpush(pool, lamb_node_new(queue));
        }
    }

    if (!queue) {
        syslog(LOG_ERR, "can't create queue for client %s", client->addr);
        request__free_unpacked(client, NULL);
        pthread_exit(NULL);
    }

    /* Client channel initialization */
    unsigned short port = config.port + 1;
    err = lamb_child_server(&fd, config.listen, &port, NN_PAIR);
    if (err) {
        pthread_cond_signal(&cond);
        request__free_unpacked(client, NULL);
        syslog(LOG_ERR, "There are no ports available for the operating system");
        pthread_exit(NULL);
    }

    pthread_mutex_lock(&mutex);

    memset(host, 0, sizeof(host));
    resp.id = client->id;
    snprintf(host, sizeof(host), "tcp://%s:%d", config.listen, port);
    resp.host = host;

    pthread_mutex_unlock(&mutex);
    pthread_cond_signal(&cond);

    timeout = config.timeout;
    nn_setsockopt(fd, NN_SOL_SOCKET, NN_RCVTIMEO, &timeout, sizeof(timeout));
    
    /* Start event processing */
    int rc;
    Report *rpack;
    Deliver *dpack;
    lamb_report_t *r;
    lamb_deliver_t *d;
    char *buf = NULL;

    while (true) {
        rc = nn_recv(fd, &buf, NN_MSG, 0);
        
        if (rc < HEAD) {
            if (rc > 0) {
                nn_freemsg(buf);
            }

            if (nn_errno() == ETIMEDOUT) {
                if (!nn_get_statistic(fd, NN_STAT_CURRENT_CONNECTIONS)) {
                    break;
                }
            }

            continue;
        }

        if (CHECK_COMMAND(buf) == LAMB_REPORT) {
            rpack = report__unpack(NULL, rc - HEAD, (uint8_t *)(buf + HEAD));
            nn_freemsg(buf);

            if (!rpack) {
                continue;
            }

            r = (lamb_report_t *)calloc(1, sizeof(lamb_report_t));

            if (r) {
                r->type = LAMB_REPORT;
                r->id = rpack->id;
                r->account = rpack->account;
                r->company = rpack->company;
                strncpy(r->spcode, rpack->spcode, 20);
                strncpy(r->phone, rpack->phone, 11);
                r->status = rpack->status;
                strncpy(r->submittime, rpack->submittime, 10);
                strncpy(r->donetime, rpack->donetime, 10);
                lamb_queue_push(queue, r);
            }

            report__free_unpacked(rpack, NULL);
            continue;
        }

        if (CHECK_COMMAND(buf) == LAMB_DELIVER) {
            dpack = deliver__unpack(NULL, rc - HEAD, (uint8_t *)(buf + HEAD));
            nn_freemsg(buf);

            if (!dpack) {
                continue;
            }

            d = (lamb_deliver_t *)calloc(1, sizeof(lamb_deliver_t));

            if (d) {
                d->type = LAMB_DELIVER;
                d->id = dpack->id;
                d->account = dpack->account;
                d->company = dpack->company;
                strncpy(d->phone, dpack->phone, 11);
                strncpy(d->spcode, dpack->spcode, 20);
                strncpy(d->serviceid, dpack->serviceid, 10);
                d->msgfmt = dpack->msgfmt;
                d->length = dpack->length;
                memcpy(d->content, dpack->content.data, dpack->content.len);
                lamb_queue_push(queue, d);
            }

            deliver__free_unpacked(dpack, NULL);
            continue;
        }

        if (CHECK_COMMAND(buf) == LAMB_BYE) {
            nn_freemsg(buf);
            break;
        }

        nn_freemsg(buf);
    }

    nn_close(fd);
    syslog(LOG_INFO, "connection closed from %s", client->addr);
    lamb_debug("connection closed from %s\n", client->addr);
    request__free_unpacked(client, NULL);

    pthread_exit(NULL);
}

void *lamb_pull_loop(void *arg) {
    int fd;
    int err;
    int timeout;
    char host[64];
    lamb_node_t *node;
    lamb_queue_t *queue;
    Request *client;
    
    client = (Request *)arg;

    lamb_debug("new client from %s connectd\n", client->addr);

    /* Client queue initialization */
    node = lamb_list_find(pool, (void *)(intptr_t)client->id);

    if (node) {
        queue = (lamb_queue_t *)node->val;
    } else {
        queue = lamb_queue_new(client->id);
        if (queue) {
            lamb_list_rpush(pool, lamb_node_new(queue));
        }
    }

    if (!queue) {
        syslog(LOG_ERR, "can't create queue for client %s", client->addr);
        request__free_unpacked(client, NULL);
        pthread_exit(NULL);
    }
    
    /* Client channel initialization */
    unsigned short port = config.port + 1;
    err = lamb_child_server(&fd, config.listen, &port, NN_REP);
    if (err) {
        pthread_cond_signal(&cond);
        request__free_unpacked(client, NULL);
        syslog(LOG_ERR, "There are no ports available for the operating system");
        pthread_exit(NULL);
    }

    pthread_mutex_lock(&mutex);

    memset(host, 0, sizeof(host));
    resp.id = client->id;
    snprintf(host, sizeof(host), "tcp://%s:%d", config.listen, port);
    resp.host = host;

    pthread_mutex_unlock(&mutex);
    pthread_cond_signal(&cond);

    timeout = config.timeout;
    nn_setsockopt(fd, NN_SOL_SOCKET, NN_RCVTIMEO, &timeout, sizeof(timeout));
    
    /* Start event processing */
    void *pk;
    int rc, len;
    void *message;
    lamb_report_t *report;
    lamb_deliver_t *deliver;
    Report rpack = REPORT__INIT;
    Deliver dpack = DELIVER__INIT;

    while (true) {
        char *buf = NULL;
        rc = nn_recv(fd, &buf, NN_MSG, 0);
        
        if (rc < HEAD) {
            if (rc > 0) {
                nn_freemsg(buf);
            }

            if (nn_errno() == ETIMEDOUT) {
                if (!nn_get_statistic(fd, NN_STAT_CURRENT_CONNECTIONS)) {
                    break;
                }
            }

            continue;
        }

        if (CHECK_COMMAND(buf) == LAMB_REQ) {
            nn_freemsg(buf);
            node = lamb_queue_pop(queue);

            if (!node) {
                len = lamb_pack_assembly(&buf, LAMB_EMPTY, NULL, 0);
                nn_send(fd, buf, len, NN_DONTWAIT);

                if (len > 0) {
                    free(buf);
                }

                continue;
            }

            message = node->val;

            if (CHECK_TYPE(message) == LAMB_REPORT) {
                report = (lamb_report_t *)message;

                rpack.id = report->id;
                rpack.account = report->account;
                rpack.company = report->company;
                rpack.spcode = report->spcode;
                rpack.phone = report->phone;
                rpack.status = report->status;
                rpack.submittime = report->submittime;
                rpack.donetime = report->donetime;

                len = report__get_packed_size(&rpack);
                pk = malloc(len);

                if (pk) {
                    report__pack(&rpack, pk);
                    len = lamb_pack_assembly(&buf, LAMB_REPORT, pk, len);
                    if (len > 0) {
                        nn_send(fd, buf, len, NN_DONTWAIT);
                        free(buf);
                    }
                    free(pk);
                }
            } else if (CHECK_TYPE(message) == LAMB_DELIVER) {
                deliver = (lamb_deliver_t *)message;

                dpack.id = deliver->id;
                dpack.account = deliver->account;
                dpack.company = deliver->company;
                dpack.phone = deliver->phone;
                dpack.spcode = deliver->spcode;
                dpack.serviceid = deliver->serviceid;
                dpack.msgfmt = deliver->msgfmt;
                dpack.length = deliver->length;
                dpack.content.len = deliver->length;
                dpack.content.data = (uint8_t *)deliver->content;

                len = deliver__get_packed_size(&dpack);
                pk = malloc(len);

                if (pk) {
                    deliver__pack(&dpack, pk);
                    len = lamb_pack_assembly(&buf, LAMB_DELIVER, pk, len);
                    if (len > 0) {
                        nn_send(fd, buf, len, NN_DONTWAIT);
                        free(buf);
                    }
                    free(pk);
                }
            }

            free(message);
            free(node);
            continue;
        }

        if (CHECK_COMMAND(buf) == LAMB_BYE) {
            nn_freemsg(buf);
            break;
        }

        nn_freemsg(buf);
    }

    nn_close(fd);
    lamb_debug("connection closed from %s\n", client->addr);
    syslog(LOG_INFO, "connection closed from %s", client->addr);
    request__free_unpacked(client, NULL);

    pthread_exit(NULL);
}

int lamb_server_init(int *sock, const char *listen, int port) {
    int fd;
    char addr[128];

    snprintf(addr, sizeof(addr), "tcp://%s:%d", listen, port);
    
    fd = nn_socket(AF_SP, NN_REP);
    if (fd < 0) {
        syslog(LOG_ERR, "socket %s", nn_strerror(nn_errno()));
        return -1;
    }

    if (nn_bind(fd, addr) < 0) {
        nn_close(fd);
        syslog(LOG_ERR, "bind %s", nn_strerror(nn_errno()));
        return -1;
    }

    *sock = fd;

    return 0;
}

int lamb_child_server(int *sock, const char *host, unsigned short *port, int protocol) {
    while (true) {
        if (!lamb_nn_server(sock, host, *port, protocol)) {
            break;
        }
        (*port)++;
    }

    return 0;
}

void *lamb_stat_loop(void *arg) {
    lamb_node_t *node;
    lamb_queue_t *queue;

    /* Reset mt queue */
    lamb_reset_queues(rdb);
    
    while (true) {
        lamb_list_iterator_t *it;
        it = lamb_list_iterator_new(pool, LIST_HEAD);
        
        while ((node = lamb_list_iterator_next(it))) {
            queue = (lamb_queue_t *)node->val;
            lamb_sync_update(rdb, queue->id, queue->list->len);
        }

        lamb_list_iterator_destroy(it);
        lamb_sleep(3000);
    }

    pthread_exit(NULL);
}

int lamb_sync_update(lamb_cache_t *cache, int id, unsigned int num) {
    redisReply *reply = NULL;

    if (!cache) {
        return -1;
    }

    reply = redisCommand(cache->handle, "HSET mo.queue %d %u", id, num);

    if (reply != NULL) {
        freeReplyObject(reply);
        return 0;
    }

    return -1;
}

void lamb_reset_queues(lamb_cache_t *cache) {
    redisReply *reply = NULL;

    reply = redisCommand(cache->handle, "DEL mo.queue");
    if (reply != NULL) {
        freeReplyObject(reply);
    }

    return;
}

int lamb_read_config(lamb_config_t *conf, const char *file) {
    if (!conf) {
        return -1;
    }

    config_t cfg;
    if (lamb_read_file(&cfg, file) != 0) {
        fprintf(stderr, "Can't open the %s configuration file\n", file);
        goto error;
    }

    /* Id */
    if (lamb_get_int(&cfg, "Id", &conf->id) != 0) {
        fprintf(stderr, "Can't read config 'Id' parameter\n");
        goto error;
    }

    /* Debug */
    if (lamb_get_bool(&cfg, "Debug", &conf->debug) != 0) {
        fprintf(stderr, "Can't read config 'Debug' parameter\n");
        goto error;
    }

    /* Listen Address */
    if (lamb_get_string(&cfg, "Listen", conf->listen, 16) != 0) {
        fprintf(stderr, "Invalid Listen IP address\n");
        goto error;
    }

    /* Listen Port */
    if (lamb_get_int(&cfg, "Port", &conf->port) != 0) {
        fprintf(stderr, "Can't read config 'Port' parameter\n");
        goto error;
    }

    /* Timeout */
    if (lamb_get_int64(&cfg, "Timeout", &conf->timeout) != 0) {
        fprintf(stderr, "Can't read config 'Timeout' parameter\n");
        goto error;
    }

    /* Log file */
    if (lamb_get_string(&cfg, "LogFile", conf->logfile, 128) != 0) {
        fprintf(stderr, "Can't read config 'LogFile' parameter\n");
        goto error;
    }

    /* Redis Host */
    if (lamb_get_string(&cfg, "RedisHost", conf->redis_host, 16) != 0) {
        fprintf(stderr, "Can't read config 'RedisHost' parameter\n");
        goto error;
    }

    /* Redis Port */
    if (lamb_get_int(&cfg, "RedisPort", &conf->redis_port) != 0) {
        fprintf(stderr, "Can't read config 'RedisPort' parameter\n");
        goto error;
    }

    /* Check redis port */
    if (conf->redis_port < 1 || conf->redis_port > 65535) {
        fprintf(stderr, "Invalid redis port number\n");
        goto error;
    }

    /* Redis Password */
    if (lamb_get_string(&cfg, "RedisPassword", conf->redis_password, 64) != 0) {
        fprintf(stderr, "Can't read config 'RedisPassword' parameter\n");
        goto error;
    }

    /* Redis database number */
    if (lamb_get_int(&cfg, "RedisDb", &conf->redis_db) != 0) {
        fprintf(stderr, "Can't read config 'RedisDb' parameter\n");
        goto error;
    }

    /* Ac */
    if (lamb_get_string(&cfg, "Ac", conf->ac, 128) != 0) {
        fprintf(stderr, "Can't read config 'Ac' parameter\n");
    }

    lamb_config_destroy(&cfg);
    return 0;
error:
    lamb_config_destroy(&cfg);
    return -1;
}

