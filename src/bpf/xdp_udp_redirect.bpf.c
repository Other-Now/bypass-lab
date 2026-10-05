// SPDX-License-Identifier: GPL-2.0
//
// XDP program for the AF_XDP path: redirect IPv4/UDP datagrams addressed to the
// feed port into the AF_XDP socket bound to this RX queue; pass everything else
// (SSH, ARP, DHCP ...) to the normal stack. Without the filter, binding AF_XDP
// to the only queue of a NIC would also swallow the management traffic.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

// Key 0: feed UDP port in host order, written by bl_rx before attach.
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} cfg_map SEC(".maps");

SEC("xdp")
int xdp_udp_redirect(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    void* end = (void*)(long)ctx->data_end;

    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > end) return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

    struct iphdr* ip = (void*)(eth + 1);
    if ((void*)(ip + 1) > end) return XDP_PASS;
    if (ip->protocol != IPPROTO_UDP || ip->ihl < 5) return XDP_PASS;

    struct udphdr* udp = (void*)ip + ip->ihl * 4;
    if ((void*)(udp + 1) > end) return XDP_PASS;

    __u32 key = 0;
    __u32* port = bpf_map_lookup_elem(&cfg_map, &key);
    if (!port || udp->dest != bpf_htons((__u16)*port)) return XDP_PASS;

    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char LICENSE[] SEC("license") = "GPL";
