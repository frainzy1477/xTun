#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "uv.h"
#include "logger.h"
#include "util.h"
#include "checksum.h"
#include "dns.h"
#include "tun.h"
#include "android.h"


struct dns_query {
    int tunfd;
    struct iphdr iphdr;
    struct udphdr udphdr;
    uv_udp_t handle;
    uv_timer_t *timer;
};

struct query_cache {
	struct dns_query *query;
	struct query_cache *next;
};

#define DNS_ANSWER_SIZE 1024
#define TIMEOUT 60
#define HASHSIZE 256

static struct query_cache *caches[HASHSIZE];

static void dns_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void dns_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags);
static void close_query(struct dns_query *query);
static void handle_local_dns_answer(struct dns_query *query, uint8_t *buf, size_t len);


static uint16_t
hash_query(uint16_t port) {
	uint32_t a = port >> 8;
	uint32_t b = port;
	return (a + b) % HASHSIZE;
}

static struct query_cache *
cache_lookup(uint16_t port) {
	int h = hash_query(port);
	struct query_cache *cache = caches[h];
	if (cache == NULL)
		return NULL;
	if (cache->query->udphdr.source == port)
		return cache;
	struct query_cache *last = cache;
	while (last->next) {
		cache = last->next;
        if (cache->query->udphdr.source == port) {
			return cache;
		}
		last = cache;
	}
	return NULL;
}

static void
cache_remove(uint16_t port) {
	int h = hash_query(port);
	struct query_cache *cache = caches[h];
	if (cache == NULL)
		return;
	if (cache->query->udphdr.source == port) {
		caches[h] = cache->next;
		return;
    }
	struct query_cache *last = cache;
	while (last->next) {
		cache = last->next;
        if (cache->query->udphdr.source == port) {
			last->next = cache->next;
		}
		last = cache;
	}
}

static void
cache_insert(uint16_t port, struct dns_query *query) {
	int h = hash_query(port);
	struct query_cache *cache = malloc(sizeof(struct query_cache));
	memset(cache, 0, sizeof(*cache));
    cache->query = query;
	cache->next = caches[h];
	caches[h] = cache;
}

void
clear_dns_query() {
	for (int i = 0; i < HASHSIZE; i++) {
        struct query_cache *cache  = caches[i];
        while (cache) {
            void *tmp = cache;
            close_query(cache->query);
            cache = cache->next;
            free(tmp);
        }
		caches[i] = NULL;
	}
}

static struct dns_query *
new_query(int tunfd, struct iphdr *iphdr, struct udphdr *udphdr) {
    struct dns_query *query = malloc(sizeof(struct dns_query));
    memset(query, 0 , sizeof(struct dns_query));
    query->iphdr = *iphdr;
    query->udphdr = *udphdr;
    query->tunfd = tunfd;
    query->timer = malloc(sizeof(uv_timer_t));

    uv_udp_init(uv_default_loop(), &query->handle);
    uv_timer_init(uv_default_loop(), query->timer);
    uv_udp_recv_start(&query->handle, dns_alloc_cb, dns_recv_cb);

    uv_os_fd_t fd = 0;
    int rc = uv_fileno((uv_handle_t*) &query->handle, &fd);
    if (rc) {
        logger_log(LOG_ERR, "Get fileno error: %s", uv_strerror(rc));
        free(query->timer);
        free(query);
        return NULL;
    } else {
        protectSocket(fd);
    }

    return query;
}

static void
timer_close_cb(uv_handle_t *handle) {
    free(handle);
}

static void
close_query(struct dns_query *query) {
    uv_close((uv_handle_t *)&query->handle, NULL);
    uv_close((uv_handle_t *)query->timer, timer_close_cb);
    free(query);
}

static void
timer_expire(uv_timer_t *handle) {
    struct dns_query *query = handle->data;
    cache_remove(query->udphdr.source);
    close_query(query);
}

static void
reset_timer(struct dns_query *query) {
    query->timer->data = query;
    uv_timer_start(query->timer, timer_expire, TIMEOUT * 1000, 0);
}

static void
dns_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(DNS_ANSWER_SIZE);
    buf->len = DNS_ANSWER_SIZE;
}

static void
dns_send_cb(uv_udp_send_t *req, int status) {
    if (status) {
        logger_log(LOG_ERR, "DNS query failed: %s", uv_strerror(status));
    }
    uv_buf_t *buf = (uv_buf_t *)(req + 1);
    free(buf->base - sizeof(struct iphdr) - sizeof(struct udphdr) - PRIMITIVE_BYTES);
    free(req);
}

static void
dns_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
    if (nread > 0) {
        struct dns_query *query = container_of(handle, struct dns_query, handle);
        reset_timer(query);
        handle_local_dns_answer(query, (uint8_t*)buf->base, nread);
    }
    free(buf->base);
}

static void
cache_log(struct iphdr *iphdr, struct udphdr *udphdr, struct sockaddr *server, const char *hint) {
    char saddr[24] = {0}, daddr[30] = {0};
    char *addr = inet_ntoa(*(struct in_addr *) &iphdr->saddr);
    strcpy(saddr, addr);
    uv_ip4_name((const struct sockaddr_in *) server, daddr, sizeof(daddr));
    logger_log(LOG_WARNING, "DNS Cache %s: %s:%d -> %s", hint, saddr, ntohs(udphdr->source), daddr);
}

int
handle_local_dns_query(int tunfd, struct sockaddr *dns_server, uint8_t *buf, int buflen) {
    struct iphdr *iphdr = (struct iphdr *)buf;
    struct udphdr *udphdr = (struct udphdr *)(buf + sizeof(struct iphdr));

    buf += sizeof(struct iphdr) + sizeof(struct udphdr);
    buflen -= sizeof(struct iphdr) + sizeof(struct udphdr);

    int rc = filter_query(buf, buflen);

    if (!rc) {
        return 0;
    }

    struct dns_query *query = NULL;
    struct query_cache *cache = cache_lookup(udphdr->source);
    if (cache == NULL) {
        query = new_query(tunfd, iphdr, udphdr);
        if (query) {
            cache_insert(udphdr->source, query);
        } else {
            return 0;
        }

    } else {
        query = cache->query;
        query->iphdr = *iphdr;
        query->udphdr = *udphdr;
        cache_log(iphdr, udphdr, dns_server, "hit");
    }

    reset_timer(query);

    uv_udp_send_t *write_req = malloc(sizeof(*write_req) + sizeof(uv_buf_t));
    uv_buf_t *outbuf = (uv_buf_t *)(write_req + 1);
    outbuf->base = (char *)buf;
    outbuf->len = buflen;
    uv_udp_send(write_req, &query->handle, outbuf, 1, dns_server, dns_send_cb);

    return 1;
}

static void
create_dns_packet(struct dns_query *query, uint8_t *answer, ssize_t answer_len, uint8_t *packet) {
    struct iphdr iphdr;
    struct udphdr udphdr;
    memset(&iphdr, 0, sizeof(iphdr));
    memset(&udphdr, 0, sizeof(udphdr));

    iphdr.protocol = query->iphdr.protocol;
    iphdr.version = query->iphdr.version;
    iphdr.daddr = query->iphdr.saddr;
    iphdr.saddr = query->iphdr.daddr;

    iphdr.id = query->iphdr.id;
    iphdr.frag_off = query->iphdr.frag_off;
    iphdr.ihl = query->iphdr.ihl;
    iphdr.ttl = query->iphdr.ttl;
    iphdr.tos = query->iphdr.tos;

    iphdr.tot_len = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + answer_len);
    iphdr.check = checksum((uint16_t*)&iphdr, sizeof(struct iphdr));

    udphdr.dest = query->udphdr.source;
    udphdr.source = query->udphdr.dest;
    udphdr.len = htons(sizeof(struct udphdr) + answer_len);
    udphdr.check = udp_checksum(&iphdr, &udphdr, answer, answer_len);;

    memcpy(packet, &iphdr, sizeof(iphdr));
    memcpy(packet + sizeof(iphdr), &udphdr, sizeof(udphdr));
    memcpy(packet + sizeof(iphdr) + sizeof(udphdr), answer, answer_len);
}

static void
handle_local_dns_answer(struct dns_query *query, uint8_t *answer, size_t answer_len) {
    int packet_len = sizeof(struct iphdr) + sizeof(struct udphdr) + answer_len;
    uint8_t packet[packet_len];

    create_dns_packet(query, answer, answer_len, packet);

    for (;;) {
        int rc = write(query->tunfd, packet, packet_len);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                logger_log(LOG_ERR, "Write tun: %s", strerror(errno));
                exit(1);
            }

        } else {
            break;
        }
    }
}