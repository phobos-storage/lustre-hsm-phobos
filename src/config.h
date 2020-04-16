/* src/include/config.h.  Generated from config.h.in by configure.  */
/* src/include/config.h.in.  Generated from configure.ac by autoheader.  */

/* Lustre changelogs records are structures */
#define HAVE_CHANGELOGS 1

/* Layout change emit changelog records */
#define HAVE_CL_LAYOUT 1

/* Define to 1 if you have the declaration of `CLF_RENAME', and to 0 if you
   don't. */
#define HAVE_DECL_CLF_RENAME 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* this version of Lustre supports DNE */
#define HAVE_DNE 1

/* File preallocation available */
#define HAVE_FALLOCATE 1

/* llapi_fd2fid function is available */
#define HAVE_FD2FID 1

/* Reentrant version of getmntent exists */
#define HAVE_GETMNTENT_R 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* define if you have zlib */
#define HAVE_LIBZ 1

/* llapi_fswap_layouts is available */
#define HAVE_LLAPI_FSWAP_LAYOUTS 1

/* llapi_getpool functions are available */
#define HAVE_LLAPI_GETPOOL_INFO 1

/* llapi_get_mdt_index_by_fid available */
#define HAVE_LLAPI_GET_MDT_INDEX_BY_FID 1

/* llapi log callbacks are available */
#define HAVE_LLAPI_LOG_CALLBACKS 1

/* llapi_msg_set_level is available */
#define HAVE_LLAPI_MSG_LEVEL 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <mysql/mysql.h> header file. */
#define HAVE_MYSQL_MYSQL_H 1

/* struct obd_stafs is defined */
#define HAVE_OBD_STATFS 1

/* lov_user_ost_data_v1 has l_object_id field */
/* #undef HAVE_OBJ_ID */

/* lov_user_ost_data_v1 has l_object_seq field */
/* #undef HAVE_OBJ_SEQ */

/* pthread_getsequence_np function exists */
/* #undef HAVE_PTHREAD_GETSEQUENCE_NP */

/* shook_lhsmify function available */
/* #undef HAVE_SHOOK_LHSMIFY */

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/param.h> header file. */
#define HAVE_SYS_PARAM_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <sys/xattr.h> header file. */
#define HAVE_SYS_XATTR_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#define LT_OBJDIR ".libs/"

/* Lustre version */
#define LUSTRE_VERSION "2.10"

/* Define to 1 if your C compiler doesn't accept -c and -o together. */
/* #undef NO_MINUS_C_MINUS_O */

/* Name of package */
#define PACKAGE "robinhood"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "robinhood-support@lists.sourceforge.net"

/* Define to the full name of this package. */
#define PACKAGE_NAME "robinhood"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "robinhood 3.1.5"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "robinhood"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "3.1.5"

/* release info */
#define RELEASE "1"

/* The size of `dev_t', as computed by sizeof. */
#define SIZEOF_DEV_T 8

/* The size of `ino_t', as computed by sizeof. */
#define SIZEOF_INO_T 8

/* The size of `nlink_t', as computed by sizeof. */
#define SIZEOF_NLINK_T 8

/* The size of `off_t', as computed by sizeof. */
#define SIZEOF_OFF_T 8

/* The size of `pthread_t', as computed by sizeof. */
#define SIZEOF_PTHREAD_T 8

/* The size of `size_t', as computed by sizeof. */
#define SIZEOF_SIZE_T 8

/* The size of `time_t', as computed by sizeof. */
#define SIZEOF_TIME_T 8

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Configuration directory */
#define SYSCONFDIR "/etc"

/* Enable extensions on AIX 3, Interix.  */
#ifndef _ALL_SOURCE
# define _ALL_SOURCE 1
#endif
/* Enable GNU extensions on systems that have them.  */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE 1
#endif
/* Enable threading extensions on Solaris.  */
#ifndef _POSIX_PTHREAD_SEMANTICS
# define _POSIX_PTHREAD_SEMANTICS 1
#endif
/* Enable extensions on HP NonStop.  */
#ifndef _TANDEM_SOURCE
# define _TANDEM_SOURCE 1
#endif
/* Enable general extensions on Solaris.  */
#ifndef __EXTENSIONS__
# define __EXTENSIONS__ 1
#endif


/* Version number of package */
#define VERSION "3.1.5"

/* Define to 1 if `lex' declares `yytext' as a `char *' by default, not a
   `char[]'. */
/* #undef YYTEXT_POINTER */

/* Enable large inode numbers on Mac OS X 10.5.  */
#ifndef _DARWIN_USE_64_BIT_INODE
# define _DARWIN_USE_64_BIT_INODE 1
#endif

/* Number of bits in a file offset, on hosts where this is settable. */
/* #undef _FILE_OFFSET_BITS */

/* CL_IOCTL is defined */
/* #undef _HAVE_CL_IOCTL */

/* lustre supports fids */
#define _HAVE_FID 1

/* HSM lite support */
#define _HSM_LITE 1

/* Define for large files, on AIX-style hosts. */
/* #undef _LARGE_FILES */

/* liblustreapi is available */
#define _LUSTRE 1

/* New lustreapi header */
#define _LUSTRE_API_HEADER 1

/* Lustre/HSM feature is present */
#define _LUSTRE_HSM 1

/* lustre_idl header exists */
/* #undef _LUSTRE_IDL_HEADER */

/* MDT LOV EA is no longer the same as lov_user_md */
#define _MDT_SPECIFIC_LOVEA 1

/* Define to 1 if on MINIX. */
/* #undef _MINIX */

/* Define to 2 if the system does not provide POSIX.1 features except with
   this defined. */
/* #undef _POSIX_1_SOURCE */

/* Define to 1 if you need to in order for `stat' and other things to work. */
/* #undef _POSIX_SOURCE */

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* Define to `int' if <sys/types.h> doesn't define. */
/* #undef gid_t */

/* Define to `__inline__' or `__inline' if that's what the C compiler
   calls it, or to nothing if 'inline' is not supported under any name.  */
#ifndef __cplusplus
/* #undef inline */
#endif

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* #undef size_t */

/* Define to `int' if <sys/types.h> doesn't define. */
/* #undef uid_t */
