FreeBSD has no SYS_gettid: freedreno's DBG() did not compile.

--- src/gallium/drivers/freedreno/freedreno_util.h.orig
+++ src/gallium/drivers/freedreno/freedreno_util.h
@@ -97,10 +97,18 @@
 #include <sys/types.h>
 #include <sys/syscall.h>
 
+#ifdef __FreeBSD__
+#include <pthread_np.h>
+/* FreeBSD has no gettid system call; its thread id */
+#define fd_gettid() ((pid_t)pthread_getthreadid_np())
+#else
+#define fd_gettid() ((pid_t)syscall(SYS_gettid))
+#endif
+
 #define DBG(fmt, ...)                                                          \
    do {                                                                        \
       if (FD_DBG(MSGS))                                                        \
-         mesa_logd("%5d: %s:%d: " fmt, ((pid_t)syscall(SYS_gettid)),           \
+         mesa_logd("%5d: %s:%d: " fmt, fd_gettid(),                           \
                                         __func__, __LINE__,                    \
                                         ##__VA_ARGS__);                        \
    } while (0)
