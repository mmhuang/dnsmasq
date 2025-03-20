#include "dnsmasq.h"
#include <hiredis/hiredis.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static redisContext* redis_connect(void) 
{
  if (!daemon->redis_url)
    return NULL;

  redisContext *c = redisConnect("localhost", 6379);
  if (c == NULL || c->err) {
    if (c) {
      my_syslog(LOG_ERR, _("Redis connection error: %s"), c->errstr);
      redisFree(c);
    }
    return NULL;
  }
  return c;
}

void redis_init_pool(void)
{
  if (!daemon->redis_url)
    return;

  daemon->redis_pool = safe_malloc(sizeof(struct redis_conn_pool));
  daemon->redis_pool->curr_conn = 0;

  for (int i = 0; i < REDIS_POOL_SIZE; i++) {
    pthread_mutex_init(&daemon->redis_pool->locks[i], NULL);
    daemon->redis_pool->conns[i] = redis_connect();
  }
}

void redis_free_pool(void)
{
  if (!daemon->redis_pool)
    return;

  for (int i = 0; i < REDIS_POOL_SIZE; i++) {
    if (daemon->redis_pool->conns[i])
      redisFree(daemon->redis_pool->conns[i]);
    pthread_mutex_destroy(&daemon->redis_pool->locks[i]);
  }
  free(daemon->redis_pool);
}

static redisContext* get_redis_conn(void)
{
  if (!daemon->redis_pool)
    return NULL;

  int idx = (daemon->redis_pool->curr_conn++) % REDIS_POOL_SIZE;
  pthread_mutex_lock(&daemon->redis_pool->locks[idx]);
  return daemon->redis_pool->conns[idx];
}

static void release_redis_conn(int idx)
{
  pthread_mutex_unlock(&daemon->redis_pool->locks[idx]);
}

void redis_store_dns_record(char *domain, union all_addr *addr, int ttl)
{
  if (!daemon->redis_url || !daemon->redis_cache_dsn_ttl)
    return;

  char ipbuf[INET6_ADDRSTRLEN];
  time_t now = time(NULL);
  int key_suffix = (now / 86400) % 2;
  int conn_idx = daemon->redis_pool->curr_conn % REDIS_POOL_SIZE;
  redisContext *redis = get_redis_conn();

  if (!redis)
    return;

  /* Determine if IPv4 or IPv6 from the address itself */
  if (IN6_IS_ADDR_V4MAPPED(&addr->addr6)) {
    /* Handle IPv4 */
    inet_ntop(AF_INET, &addr->addr4, ipbuf, sizeof(ipbuf));
  } else {
    /* Handle IPv6 */
    inet_ntop(AF_INET6, &addr->addr6, ipbuf, sizeof(ipbuf));
  }

  /* Store in current suffix - use TTL for key expiration */
  redisCommand(redis, "SADD dns%d:%s %s", key_suffix + 1, domain, ipbuf);
  
  /* Set expiration based on TTL parameter or default TTL from config */
  int expiry = ttl > 0 ? ttl : daemon->redis_cache_dsn_ttl;
  redisCommand(redis, "EXPIRE dns%d:%s %d", key_suffix + 1, domain, expiry);

  /* Double write near rotation time */
  if (now % 86400 >= 82800) {
    redisCommand(redis, "SADD dns%d:%s %s", (key_suffix + 1) % 2 + 1, 
                 domain, ipbuf);
    redisCommand(redis, "EXPIRE dns%d:%s %d", (key_suffix + 1) % 2 + 1,
                 domain, expiry);
  }

  release_redis_conn(conn_idx);
}

void redis_store_ptr_record(char *ip, char *domain, int ttl) 
{
  if (!daemon->redis_url || !daemon->redis_cache_ptr_ttl)
    return;

  time_t now = time(NULL);
  int key_suffix = (now / 259200) % 2;  /* 3 day rotation */
  int conn_idx = daemon->redis_pool->curr_conn % REDIS_POOL_SIZE;
  redisContext *redis = get_redis_conn();

  if (!redis)
    return;

  /* Use TTL parameter if provided, otherwise use the configured default */
  int expiry = ttl > 0 ? ttl : daemon->redis_cache_ptr_ttl;

  redisCommand(redis, "SADD ptr%d:%s %s", key_suffix + 1, ip, domain);
  redisCommand(redis, "EXPIRE ptr%d:%s %d", key_suffix + 1, ip, expiry);

  /* Double write near rotation time */
  if (now % 259200 >= 252000) {
    redisCommand(redis, "SADD ptr%d:%s %s", (key_suffix + 1) % 2 + 1, 
                ip, domain);
    redisCommand(redis, "EXPIRE ptr%d:%s %d", (key_suffix + 1) % 2 + 1,
                ip, expiry);
  }

  release_redis_conn(conn_idx);
}
