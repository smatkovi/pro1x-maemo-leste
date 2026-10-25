/*
 * ld-linux.so.3: Harmattan's name of the dynamic loader, as a library.
 *
 * Programs and libraries built with newer ARM toolchains name the loader
 * in DT_NEEDED (for __tls_get_addr or __stack_chk_guard), under its
 * Harmattan/armel name ld-linux.so.3. Debian's armhf loader runs them but
 * only knows itself as ld-linux-armhf.so.3, so it would map a second copy
 * of itself for that entry, whose libc start-up then dies with SIGFPE.
 *
 * This stub takes the name instead. It defines the symbol versions such
 * objects require from ld-linux.so.3 (GLIBC_2.4, GLIBC_PRIVATE); the
 * symbols themselves come from the real loader, which libc pulls in.
 */
int __pro1x_ld_linux_compat;
int __pro1x_ld_linux_compat_private;
