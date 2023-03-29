AC_PREREQ([2.69])
AC_INIT([disptach], [0.1])
AC_CONFIG_AUX_DIR([aux])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall -Werror])

# Enable non-standard functions and other goodies
AC_USE_SYSTEM_EXTENSIONS

# Checks for programs.
AC_PROG_CC
AM_PROG_AR

# Initialise libtool
LT_INIT

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([stdlib.h])
AC_CHECK_HEADERS([unistd.h sys/types.h sys/stat.h sys/wait.h fcntl.h poll.h signal.h sys/signalfd.h time.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([dup2 sigprocmask signalfd])
AC_CHECK_FUNCS([memmove strcasecmp strtol])

# Output
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
