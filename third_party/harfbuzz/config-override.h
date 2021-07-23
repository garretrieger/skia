/*
 * Adds a mutex implementation based on c++ mutex to harfbuzz.
 */
#include <mutex>

typedef std::mutex              hb_mutex_impl_t;
#define HB_MUTEX_IMPL_INIT      0
#define hb_mutex_impl_init(M)   HB_STMT_START {} HB_STMT_END
#define hb_mutex_impl_lock(M)   (*(M)).lock ()
#define hb_mutex_impl_unlock(M) (*(M)).unlock ()
#define hb_mutex_impl_finish(M) HB_STMT_START {} HB_STMT_END

