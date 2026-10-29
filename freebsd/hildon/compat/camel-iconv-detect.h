/* camel's iconv-detect.h (camel-lite in tinymail, camel in Evolution Data
 * Server) for FreeBSD's iconv (libc, citrus): what iconv-detect, which a
 * cross build cannot run, prints there (citrus takes charset names in any
 * case; UCS-4BE for ISO 10646) */

#define ICONV_ISO_D_FORMAT "iso-%d-%d"
#define ICONV_ISO_S_FORMAT "iso-%d-%s"
#define ICONV_10646 "UCS-4BE"
