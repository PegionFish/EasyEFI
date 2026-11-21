/* 宿主原生 jansson 配置（S03 手写，替代 autotools 生成）。
   源码 = 共享 edk2 子模块 jansson 2.13.1（与 UEFI JsonLib 同版本）。
   与 UEFI 版（RedfishPkg/Library/JsonLib/jansson_config.h）的差异：
   宿主有 inline/locale/atomic builtins。JSON_INTEGER_IS_LONG_LONG=1 与 UEFI 一致
   （json_int_t = long long = INT64，DOM 数值语义对齐）。 */
#ifndef JANSSON_CONFIG_H
#define JANSSON_CONFIG_H

#define JSON_INLINE inline
#define JSON_INTEGER_IS_LONG_LONG 1
#define JSON_HAVE_LOCALECONV 1
#define JSON_HAVE_ATOMIC_BUILTINS 1
#define JSON_HAVE_SYNC_BUILTINS 1
#define JSON_PARSER_MAX_DEPTH 2048

#endif