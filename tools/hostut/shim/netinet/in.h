/* 主机侧单元测试用最小 shim：只补齐 cm_rtp.c 用得到的类型声明。
 * 不参与真机构建（真机走 zig -target aarch64-linux-gnu，有真正的 glibc 头）。 */
#ifndef UT_SHIM_NETINET_IN_H
#define UT_SHIM_NETINET_IN_H
#include <stdint.h>
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    uint16_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
#endif
