#include "dnsmasq.h"
#include <hiredis/hiredis.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

// static redisContext* redis_connect(void) 
// {
//   if (!daemon->redis_url)
//     return NULL;

//   redisContext *c = redisConnect("localhost", 6379);
//   if (c == NULL || c->err) {
//     if (c) {
//       my_syslog(LOG_ERR, _("Redis connection error: %s"), c->errstr);
//       redisFree(c);
//     }
//     return NULL;
//   }
//   return c;
// }

static redisContext* redis_connect(void) 
{
  if (!daemon->redis_url)
    return NULL;

  // Parse redis URL in format:
  // redis://[:password@]host[:port][/database][?[timeout=timeout]]
  char *url_copy = strdup(daemon->redis_url);
  if (!url_copy) {
    my_syslog(LOG_ERR, _("Failed to allocate memory for Redis URL"));
    return NULL;
  }

  // Default values
  char *host = "localhost";
  int port = 6379;
  char *password = NULL;
  int database = 0;
  struct timeval timeout = {1, 500000}; // 1.5 seconds default timeout

  // Extract protocol prefix (redis://)
  if (strncmp(url_copy, "redis://", 8) == 0) {
    char *ptr = url_copy + 8; // Skip "redis://"
    
    // Extract password if present
    char *at_sign = strchr(ptr, '@');
    if (at_sign) {
      *at_sign = '\0'; // Split string at @ sign
      password = ptr;
      ptr = at_sign + 1; // Move past the @ sign
    }
    
    // Extract host
    host = ptr;
    
    // Extract port if present
    char *colon = strchr(ptr, ':');
    if (colon) {
      *colon = '\0'; // Split string at colon
      port = atoi(colon + 1);
      
      // Check for database or query parameters
      char *slash = strchr(colon + 1, '/');
      if (slash) {
        *slash = '\0'; // Terminate port string
        port = atoi(colon + 1);
        
        // Extract database number
        char *question = strchr(slash + 1, '?');
        if (question) {
          *question = '\0'; // Terminate database string
          if (slash + 1 != question) { // If there's content between / and ?
            database = atoi(slash + 1);
          }
          
          // Parse query parameters (timeout, etc)
          if (strstr(question + 1, "timeout=")) {
            char *timeout_val = strstr(question + 1, "timeout=") + 8;
            float timeout_sec = atof(timeout_val);
            timeout.tv_sec = (int)timeout_sec;
            timeout.tv_usec = (int)((timeout_sec - timeout.tv_sec) * 1000000);
          }
        } else if (*(slash + 1)) {
          database = atoi(slash + 1);
        }
      }
    } else {
      // No port specified, check for database
      char *slash = strchr(ptr, '/');
      if (slash) {
        *slash = '\0'; // Terminate host string
        
        // Extract database number
        char *question = strchr(slash + 1, '?');
        if (question) {
          *question = '\0'; // Terminate database string
          if (slash + 1 != question) { // If there's content between / and ?
            database = atoi(slash + 1);
          }
          
          // Parse query parameters (timeout, etc)
          if (strstr(question + 1, "timeout=")) {
            char *timeout_val = strstr(question + 1, "timeout=") + 8;
            float timeout_sec = atof(timeout_val);
            timeout.tv_sec = (int)timeout_sec;
            timeout.tv_usec = (int)((timeout_sec - timeout.tv_sec) * 1000000);
          }
        } else if (*(slash + 1)) {
          database = atoi(slash + 1);
        }
      }
    }
  }

  // Connect with appropriate parameters
  redisContext *c;
  if (timeout.tv_sec || timeout.tv_usec) {
    c = redisConnectWithTimeout(host, port, timeout);
  } else {
    c = redisConnect(host, port);
  }
  
  if (c == NULL || c->err) {
    if (c) {
      my_syslog(LOG_ERR, _("Redis connection error: %s"), c->errstr);
      redisFree(c);
    } else {
      my_syslog(LOG_ERR, _("Redis connection error: can't allocate redis context"));
    }
    free(url_copy);
    return NULL;
  }
  
  // Authenticate if password was provided
  if (password && strlen(password) > 0) {
    redisReply *reply = redisCommand(c, "AUTH %s", password);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
      my_syslog(LOG_ERR, _("Redis authentication failed"));
      if (reply)
        freeReplyObject(reply);
      redisFree(c);
      free(url_copy);
      return NULL;
    }
    freeReplyObject(reply);
  }
  
  // Select database if specified
  if (database > 0) {
    redisReply *reply = redisCommand(c, "SELECT %d", database);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
      my_syslog(LOG_ERR, _("Redis database selection failed"));
      if (reply)
        freeReplyObject(reply);
      redisFree(c);
      free(url_copy);
      return NULL;
    }
    freeReplyObject(reply);
  }
  
  free(url_copy);
  my_syslog(LOG_INFO, _("Connected to Redis at %s:%d"), host, port);
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

void redis_store_dns_record(char *domain, char *ip, int ttl)
{
  if (!daemon->redis_url || !daemon->redis_cache_dns_ttl)
    return;

  time_t now = time(NULL);
  int key_suffix = (now / ttl) % 2;
  int conn_idx = daemon->redis_pool->curr_conn % REDIS_POOL_SIZE;
  redisContext *redis = get_redis_conn();

  if (!redis)
    return;

  /* Store in current suffix - use TTL for key expiration */
  redisCommand(redis, "SADD dns%d:%s %s", key_suffix + 1, domain, ip);
  
  /* Set expiration based on TTL parameter or default TTL from config */
  int expiry = ttl > 0 ? ttl : daemon->redis_cache_dns_ttl;
  redisCommand(redis, "EXPIRE dns%d:%s %d", key_suffix + 1, domain, expiry);

  /* Double write near rotation time */
  if (now % 86400 >= 82800) {
    redisCommand(redis, "SADD dns%d:%s %s", (key_suffix + 1) % 2 + 1, 
                 domain, ip);
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
  int key_suffix = (now / ttl ) % 2;  /* 3 day rotation */
  int conn_idx = daemon->redis_pool->curr_conn % REDIS_POOL_SIZE;
  redisContext *redis = get_redis_conn();

  if (!redis)
    return;

  /* Use TTL parameter if provided, otherwise use the configured default */
  int expiry = ttl > 0 ? ttl : daemon->redis_cache_ptr_ttl;

  redisCommand(redis, "SADD ptr%d:%s %s", key_suffix + 1, ip, domain);
  redisCommand(redis, "EXPIRE ptr%d:%s %d", key_suffix + 1, ip, expiry);

  /* Double write near rotation time */
  if (now % ttl >= ttl - 3600 ) {
    redisCommand(redis, "SADD ptr%d:%s %s", (key_suffix + 1) % 2 + 1, 
                ip, domain);
    redisCommand(redis, "EXPIRE ptr%d:%s %d", (key_suffix + 1) % 2 + 1,
                ip, expiry);
  }

  release_redis_conn(conn_idx);
}
