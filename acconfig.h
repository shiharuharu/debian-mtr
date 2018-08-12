
/* Define if you don't have the GTK+ libraries available.  */
#undef NO_GTK

/* Define if you don't have the curses libraries available.  */
#undef NO_CURSES

/* Define if you don't have the herror() function available.  */
#undef NO_HERROR

/* Define if you don't have the strerror() function available.  */
#undef NO_STRERROR

/*  Define the package name.  ("mtr")  */
#undef PACKAGE

/*  Define the version string.  */
#undef VERSION

@TOP@

@BOTTOM@

/*  Find the proper type for 8 bits  */
#if SIZEOF_UNSIGNED_CHAR == 1
typedef unsigned char uint8;
#else
#error No 8 bit type
#endif

/*  Find the proper type for 16 bits  */
#if SIZEOF_UNSIGNED_SHORT == 2
typedef unsigned short uint16;
#elif SIZEOF_UNSIGNED_INT == 2
typedef unsigned int uint16;
#elif SIZEOF_UNSIGNED_LONG == 2
typedef unsigned long uint16;
#else
#error No 16 bit type
#endif

/*  Find the proper type for 32 bits  */
#if SIZEOF_UNSIGNED_SHORT == 4
typedef unsigned short uint32;
#elif SIZEOF_UNSIGNED_INT == 4
typedef unsigned int uint32;
#elif SIZEOF_UNSIGNED_LONG == 4
typedef unsigned long uint32;
#else
#error No 32 bit type
#endif
