int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *test = argv[1];
    struct nf_conn ct = {.protocol = IPPROTO_UDP, .nat = {.masq_index = 3}};
    ct.tuplehash[0].tuple.src.u3.ip = 0x01010101;
    ct.tuplehash[0].tuple.src.u.udp.port = htons(40001);
    struct nf_conn before;
    struct sk_buff skb = {.ct = &ct};
    struct net_device out = {.ifindex = 9, .name = "test0"};
    struct nf_nat_range2 range = {0};
    unsigned int expected = NF_ACCEPT;

    if (!strncmp(test, "confirmed", 9)) {
        ct.confirmed = true;
        ct.has_help = strcmp(test, "confirmed-no-extension") != 0;
        if (!strcmp(test, "confirmed-other-helper")) ct.help.helper = &other_helper;
    } else if (!strcmp(test, "existing-helper-space")) {
        ct.has_help = true;
    } else if (!strcmp(test, "other-helper")) {
        ct.has_help = true; ct.help.helper = &other_helper;
    } else if (!strcmp(test, "mode-disabled")) {
        nf_conntrack_nat_mode = 0;
    } else if (!strcmp(test, "tcp")) {
        ct.protocol = IPPROTO_TCP;
    } else if (!strcmp(test, "zero-source")) {
        ct.tuplehash[0].tuple.src.u3.ip = 0;
    } else if (!strcmp(test, "no-address")) {
        chosen_address = 0; expected = NF_DROP;
    } else if (!strcmp(test, "setup-failure")) {
        setup_verdict = expected = NF_DROP;
    } else if (!strcmp(test, "helper-allocation-failure")) {
        fail_helper = true;
    } else if (!strcmp(test, "existing-expectation")) {
        reuse_expect = true;
        expectation.tuple.dst.u.udp.port = htons(49001);
    } else if (!strcmp(test, "last-port-free")) {
        range.min_proto.all = range.max_proto.all = htons(65535);
    } else if (!strcmp(test, "last-port-busy")) {
        range.min_proto.all = range.max_proto.all = htons(65535);
        ports_busy = true; expected = NF_DROP;
    } else if (!strcmp(test, "all-odd-ports-busy")) {
        range.min_proto.all = htons(1); range.max_proto.all = htons(65535);
        ports_busy = true; expected = NF_DROP;
    } else if (!strcmp(test, "finite-range-busy")) {
        range.min_proto.all = htons(40101); range.max_proto.all = htons(40103);
        ports_busy = true; expected = NF_DROP;
    } else if (!strcmp(test, "invalid-range")) {
        range.min_proto.all = htons(40103); range.max_proto.all = htons(40101);
        expected = NF_DROP;
    } else {
        assert(!strcmp(test, "new-udp"));
    }
    before = ct;
    unsigned int result = nf_nat_masquerade_ipv4(&skb, NF_INET_POST_ROUTING, &range, &out);
#ifdef BEFORE_FIX
    if (ct.confirmed) {
        assert(nat_calls == 1 && ct.nat.masq_index == out.ifindex);
        if (!ct.has_help || !before.has_help) assert(late_extensions == 1);
        else if (!before.help.helper) assert(ct.help.helper == &nf_conntrack_helper_bcm_nat);
        puts("OLD_CONFIRMED_CONNTRACK_MUTATED");
        return 0;
    }
#else
    if (ct.confirmed) {
        assert(result == NF_ACCEPT && !setup_calls && !nat_calls);
        assert(!helper_calls && !late_extensions && !lookup_calls && !scans);
        assert(!memcmp(&ct, &before, sizeof(ct)));
        puts("CONFIRMED_CONNTRACK_UNCHANGED");
        return 0;
    }
#endif
    assert(result == expected && lock_depth == 0 && late_extensions == 0);
    if (ports_busy || !strcmp(test, "invalid-range")) {
        assert(!setup_calls && !helper_calls);
        if (!strcmp(test, "last-port-busy")) assert(scans == 1);
        if (!strcmp(test, "all-odd-ports-busy")) assert(scans == 32768);
    } else if (!strcmp(test, "zero-source") || !strcmp(test, "no-address")) {
        assert(!setup_calls && !helper_calls && !lookup_calls);
    } else if (!nf_conntrack_nat_mode || ct.protocol == IPPROTO_TCP || before.help.helper) {
        assert(setup_calls == 1 && !helper_calls && !lookup_calls);
        assert(ct.help.helper == before.help.helper);
    } else if (setup_verdict == NF_DROP || fail_helper) {
        assert(!ct.help.helper && setup_calls == 1);
        assert(helper_calls == (unsigned int)fail_helper);
    } else {
        assert(ct.help.helper == &nf_conntrack_helper_bcm_nat && setup_calls == 1);
        assert(helper_calls == !before.has_help);
        assert(chosen_range.min_proto.all == chosen_range.max_proto.all);
        unsigned int selected = ntohs(chosen_range.min_proto.all);
        if (reuse_expect) assert(selected == 49001 && !scans);
        else if (!strcmp(test, "last-port-free")) assert(selected == 65535 && scans == 1);
        else assert(selected == 40001 && scans == 1);
    }
    puts("FULLCONE_CASE_PASS");
    return 0;
}
