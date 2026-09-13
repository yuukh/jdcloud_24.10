static void provenance_cases(void)
{
    unsigned char memory[256] = {0};
    struct sk_buff skb = {.head = memory, .data = memory + 64, .tag = true, .extge = true};
    /* A complete recycled tag, not just random invalid bytes, must be ignored. */
    for (unsigned int port = 1; port <= 6; port++) {
        skb.sport = port;
        metadata_reads = 0;
        assert(!hnat_cpu_has_ingress_port(&skb));
        assert(!hnat_cpu_has_valid_info(&skb));
        assert(!hnat_cpu_has_ingress_info(&skb));
        assert(!hnat_cpu_from_extge(&skb));
        assert(metadata_reads == 0);
        skb.skb_iif = 4;
        assert(hnat_cpu_has_ingress_port(&skb));
        assert(hnat_cpu_has_valid_info(&skb));
        assert(hnat_cpu_has_ingress_info(&skb));
        assert(hnat_cpu_from_extge(&skb));
        skb.skb_iif = 0;
    }
    skb.offload_no_fdb = true;
    assert(hnat_cpu_has_ingress_port(&skb));
    assert(hnat_cpu_has_valid_info(&skb));
    assert(hnat_cpu_has_ingress_info(&skb));
    /* The authoritative return marker blocks injection even after tag loss. */
    skb.tag = skb.extge = false;
    assert(hnat_cpu_has_ingress_port(&skb));
    assert(!hnat_cpu_has_valid_info(&skb));
    assert(!hnat_cpu_has_ingress_info(&skb));
    skb.data = memory + 2;
    metadata_reads = 0;
    assert(!hnat_cpu_has_valid_info(&skb));
    assert(!hnat_cpu_has_ingress_info(&skb));
    assert(metadata_reads == 0);
}

static struct sk_buff packet(unsigned char memory[256])
{
    struct sk_buff skb = {.head = memory, .data = memory + 64, .len = 72,
                         .network_header = 64, .protocol = htons(ETH_P_IP)};
    memset(memory, 0, 256);
    struct iphdr *ip = (void *)skb.data;
    ip->version = 4; ip->ihl = 5; ip->protocol = IPPROTO_TCP;
    ip->tot_len = htons(skb.len);
    struct tcphdr *tcp = (void *)(skb.data + sizeof(*ip));
    tcp->doff = 5; tcp->seq = htonl(12345);
    return skb;
}

static void parser_cases(void)
{
    _Alignas(16) unsigned char memory[256], before[256];
    struct sk_buff skb = packet(memory), saved;
    struct h418_packet p;
    assert(h418_decode(&skb, &p));
    assert(p.netoff == 0 && ntohl(p.tcp.seq) == 12345);
    skb.data += 20; skb.len -= 20;
    assert(h418_decode(&skb, &p) && p.netoff == -20);
    skb = packet(memory);
    memset(memory, 0xa5, 64);
    skb.network_header = 0;
    saved = skb;
    memcpy(before, memory, sizeof(memory));
    assert(!h418_decode(&skb, &p));
    assert(h418_decode_rx(&skb, &p) && p.netoff == 0);
    assert(memcmp(&skb, &saved, sizeof(skb)) == 0);
    assert(memcmp(memory, before, sizeof(memory)) == 0);
    skb.len += 8; /* Ethernet padding is accepted, IP length stays 72. */
    assert(h418_decode_rx(&skb, &p) && ntohs(p.ip.tot_len) - p.header_len == 32);
    skb.data_len = 40; /* TCP header obtained through skb_header_pointer copy. */
    assert(h418_decode_rx(&skb, &p) && ntohl(p.tcp.seq) == 12345);
    skb.data_len = 0;
    for (int depth = 1; depth <= 3; depth++) {
        skb.data -= VLAN_HLEN; skb.len += VLAN_HLEN;
        struct vlan_hdr *vlan = (void *)skb.data;
        vlan->h_vlan_TCI = htons(1234);
        vlan->h_vlan_encapsulated_proto = skb.protocol;
        skb.protocol = htons(depth == 2 ? ETH_P_8021AD : ETH_P_8021Q);
        assert(h418_decode_rx(&skb, &p) == (depth <= 2));
        if (depth <= 2) assert(p.netoff == depth * VLAN_HLEN);
    }
    skb.len = 2;
    assert(!h418_decode_rx(&skb, &p));
    assert(!h418_decode_rx(NULL, &p));
    assert(!h418_decode(NULL, &p));
    skb = packet(memory);
    struct iphdr *ip = (void *)skb.data;
    ip->tot_len = htons(1000);
    assert(!h418_decode_rx(&skb, &p));
    ip->tot_len = htons(20);
    assert(!h418_decode_rx(&skb, &p));
    ip->tot_len = htons(72); ip->frag_off = htons(0x2000);
    assert(!h418_decode_rx(&skb, &p));
    ip->frag_off = 0; skb.protocol = htons(0x86dd);
    assert(!h418_decode_rx(&skb, &p));
}

int main(void)
{
    provenance_cases();
    parser_cases();
    puts("HNAT418_DATAPATH_HELPERS_PASS");
    return 0;
}
