#ifndef SMPP_DATABASE_REDIS_H
#define SMPP_DATABASE_REDIS_H

#ifdef __cplusplus
extern "C" {
#endif

#define SMPP_REDIS_QUEUE_PUSH "LPUSH %s %s"
#define SMPP_REDIS_QUEUE_POP "LPOP %s %ld"
#define SMPP_REDIS_GLOBAL_SERVICE "_global_"

#ifdef __cplusplus
}
#endif

#endif /* SMPP_DATABASE_REDIS_H */
