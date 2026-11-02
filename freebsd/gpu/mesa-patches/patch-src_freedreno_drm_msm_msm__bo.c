FreeBSD has no ENODATA: freedreno's msm backend did not compile.

--- src/freedreno/drm/msm/msm_bo.c.orig
+++ src/freedreno/drm/msm/msm_bo.c
@@ -8,6 +8,11 @@
 
 #include "msm_priv.h"
 
+/* FreeBSD has no ENODATA (src/util/os_file.c) */
+#ifndef ENODATA
+#define ENODATA ENOATTR
+#endif
+
 static int
 bo_allocate(struct msm_bo *msm_bo)
 {
