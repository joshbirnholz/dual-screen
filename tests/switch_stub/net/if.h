// libnx has no <net/if.h>.
//
// The rest of this directory stands in for headers the Switch *does* have, so
// the host compiler can check code against their signatures. This is the
// opposite: a header every desktop has and a Switch does not, standing in so
// that including it fails here the way it fails there. Without it, the host's
// own copy is found instead and the check passes code the Switch rejects --
// which is how <netinet/ip.h> reached a Switch build and stopped it.
#error "libnx has no <net/if.h>; the Switch build cannot include it"
