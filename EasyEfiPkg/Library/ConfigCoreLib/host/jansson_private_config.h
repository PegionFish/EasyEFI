/* 宿主原生 jansson 私有配置（S03 手写；macOS/Linux 原生 clang/gcc）。
   编译需 -DHAVE_CONFIG_H=1（与 JsonLib.inf GCC flags 一致）。 */
#ifndef JANSSON_PRIVATE_CONFIG_H
#define JANSSON_PRIVATE_CONFIG_H

#define HAVE_STDINT_H     1
#define HAVE_UNISTD_H     1
#define HAVE_FCNTL_H      1
#define HAVE_SYS_STAT_H   1
#define HAVE_SYS_TIME_H   1
#define HAVE_SYS_TYPES_H  1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GETPID       1
#define HAVE_OPEN         1
#define HAVE_CLOSE        1
#define HAVE_READ         1

#define INITIAL_HASHTABLE_ORDER 3

#endif