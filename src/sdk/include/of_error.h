/*
 * of_error.h -- Unified error codes for openfpgaOS
 */

#ifndef OF_ERROR_H
#define OF_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OF_OK           =  0,
    OF_ERR_TIMEOUT  = -1,
    OF_ERR_IO       = -2,
    OF_ERR_PARAM    = -3,
    OF_ERR_BUSY     = -4,
    OF_ERR_NOSYS    = -5,
    OF_ERR_NOMEM    = -6,
} of_error_t;

#ifdef __cplusplus
}
#endif

#endif /* OF_ERROR_H */
