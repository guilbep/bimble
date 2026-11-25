/* earcutr 0.5.0 (src/lib.rs) in C; see earcut.h. Function names follow the crate. Node 0 is the
 * crate's NULL node. Nodes live in one growable array and are referred to by index, like the
 * crate's Vec, so the order of node creation (and so every tie break) is the same.
 * Floating-point contraction would change bits: it is off. */
#ifdef __clang__
#pragma clang fp contract(off)
#endif
#include "earcut.h"
#include <stdlib.h>
#include <string.h>

#define NUL 0
#define F64_MAX 1.7976931348623157e308
#define MAX_DEPTH 2000
#define STEP_BUDGET 2000000000ull

typedef struct {
    size_t i; /* vertices_index */
    double x, y;
    size_t prev, next;
    int32_t z;
    size_t prevz, nextz;
    int steiner;
} Node;

typedef struct {
    Node *n;
    size_t len, cap;
    double invsize, minx, miny, maxx, maxy;
    int usehash;
    uint32_t *tri;
    size_t ntri, tcap;
    int err;
    uint64_t steps;
} LL;

/* f64::min / f64::max (NaN ignored). The sign of a zero result never reaches a comparison. */
static double fmin64(double a, double b) {
    if (a != a) return b;
    if (b != b) return a;
    return a < b ? a : b;
}
static double fmax64(double a, double b) {
    if (a != a) return b;
    if (b != b) return a;
    return a > b ? a : b;
}
static double fabs64(double x) { return x < 0.0 ? -x : x == 0.0 ? 0.0 : x; }

static int step(LL *ll) {
    if (++ll->steps > STEP_BUDGET) {
        if (!ll->err) ll->err = CC_EARCUT_LIMIT;
        return 0;
    }
    return 1;
}

static int grow(LL *ll, size_t more) {
    if (ll->len + more <= ll->cap) return 1;
    size_t c = ll->cap ? ll->cap : 16;
    while (c < ll->len + more) c *= 2;
    if (c > ((size_t)-1) / sizeof(Node)) return 0;
    Node *p = realloc(ll->n, c * sizeof(Node));
    if (!p) return 0;
    ll->n = p;
    ll->cap = c;
    return 1;
}

static void push_tri(LL *ll, size_t a, size_t b, size_t c) {
    if (ll->err) return;
    if (ll->ntri + 3 > ll->tcap) {
        size_t t = ll->tcap ? 2 * ll->tcap : 48;
        uint32_t *p = realloc(ll->tri, t * sizeof(uint32_t));
        if (!p) {
            ll->err = CC_EARCUT_OOM;
            return;
        }
        ll->tri = p;
        ll->tcap = t;
    }
    ll->tri[ll->ntri++] = (uint32_t)a;
    ll->tri[ll->ntri++] = (uint32_t)b;
    ll->tri[ll->ntri++] = (uint32_t)c;
}

#define N(k) (ll->n[(k)])
#define NEXT(k) (ll->n[(k)].next)
#define PREV(k) (ll->n[(k)].prev)

/* Coord::zorder. 0 ok, -1 Error::Unknown (num_traits::cast to i64 failed). */
static int zorder(double x, double y, double invsize, int32_t *out) {
    double fx = x * invsize, fy = y * invsize;
    if (!(fx > -9223372036854775808.0 && fx < 9223372036854775808.0)) return -1;
    if (!(fy > -9223372036854775808.0 && fy < 9223372036854775808.0)) return -1;
    int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    uint64_t xy = ((uint64_t)ix << 32) | (uint64_t)iy;
    xy = (xy | (xy << 8)) & 0x00FF00FF00FF00FFull;
    xy = (xy | (xy << 4)) & 0x0F0F0F0F0F0F0F0Full;
    xy = (xy | (xy << 2)) & 0x3333333333333333ull;
    xy = (xy | (xy << 1)) & 0x5555555555555555ull;
    *out = (int32_t)(uint32_t)((xy >> 32) | (xy << 1));
    return 0;
}

/* NodeTriangle::area */
static double area(const Node *p, const Node *q, const Node *r) {
    return (q->y - p->y) * (r->x - q->x) - (q->x - p->x) * (r->y - q->y);
}

/* NodeTriangle::contains_point (a, b, c = self.0, self.1, self.2) */
static int contains(const Node *a, const Node *b, const Node *c, const Node *p) {
    return (c->x - p->x) * (a->y - p->y) - (a->x - p->x) * (c->y - p->y) >= 0.0 &&
           (a->x - p->x) * (b->y - p->y) - (b->x - p->x) * (a->y - p->y) >= 0.0 &&
           (b->x - p->x) * (c->y - p->y) - (c->x - p->x) * (b->y - p->y) >= 0.0;
}

static int xy_eq(const Node *a, const Node *b) { return a->x == b->x && a->y == b->y; }

/* LinkedLists::insert_node; (size_t)-1 = None. Returns the new index, 0 when out of memory. */
static size_t insert_node(LL *ll, size_t i, double x, double y, size_t last) {
    if (!grow(ll, 1)) {
        ll->err = CC_EARCUT_OOM;
        return NUL;
    }
    size_t idx = ll->len;
    Node p;
    memset(&p, 0, sizeof p);
    p.i = i;
    p.x = x;
    p.y = y;
    if (last == (size_t)-1) {
        p.next = p.prev = idx;
    } else {
        p.next = NEXT(last);
        p.prev = last;
        size_t ln = NEXT(last);
        PREV(ln) = idx;
        NEXT(last) = idx;
    }
    ll->n[ll->len++] = p;
    return idx;
}

static void remove_node(LL *ll, size_t p) {
    size_t pi = PREV(p), ni = NEXT(p), pz = N(p).prevz, nz = N(p).nextz;
    NEXT(pi) = ni;
    PREV(ni) = pi;
    N(pz).nextz = nz;
    N(nz).prevz = pz;
}

/* Simon Tatham's linked list merge sort, on z. */
static void sort_linked(LL *ll, size_t list) {
    size_t p, q, e, tail, insize = 1, nummerges, psize, qsize;
    for (;;) {
        p = list;
        list = NUL;
        tail = NUL;
        nummerges = 0;
        while (p != NUL) {
            nummerges++;
            q = p;
            psize = 0;
            while (q != NUL && psize < insize) {
                psize++;
                q = N(q).nextz;
            }
            qsize = insize;
            while (psize > 0 || (qsize > 0 && q != NUL)) {
                if (psize > 0 && (qsize == 0 || q == NUL || N(p).z <= N(q).z)) {
                    e = p;
                    p = N(p).nextz;
                    psize--;
                } else {
                    e = q;
                    q = N(q).nextz;
                    qsize--;
                }
                if (tail != NUL) N(tail).nextz = e;
                else list = e;
                N(e).prevz = tail;
                tail = e;
            }
            p = q;
        }
        N(tail).nextz = NUL;
        insize *= 2;
        if (nummerges <= 1) break;
    }
}

static int index_curve(LL *ll, size_t start) {
    size_t p = start;
    do {
        if (N(p).z == 0 && zorder(N(p).x, N(p).y, ll->invsize, &N(p).z)) return -1;
        N(p).prevz = PREV(p);
        N(p).nextz = NEXT(p);
        p = NEXT(p);
    } while (p != start);
    size_t pzi = N(start).prevz;
    N(pzi).nextz = NUL;
    N(start).prevz = NUL;
    sort_linked(ll, start);
    return 0;
}

/* Vertices::signed_area */
static double signed_area(const double *v, size_t start, size_t end) {
    double s = 0.0;
    size_t j = end - 2;
    for (size_t i = start; i < end; i += 2) {
        s = s + (v[j] - v[i]) * (v[i + 1] + v[j + 1]);
        j = i;
    }
    return s;
}

/* LinkedLists::add_contour. 0 ok, else an error code. */
static int add_contour(LL *ll, const double *v, size_t vlen, size_t start, size_t end, int clockwise,
                       size_t *last_out, size_t *left_out) {
    if (start > vlen || end > vlen || vlen == 0) return CC_EARCUT_UNKNOWN;
    if (end < 2 || end - 2 < start) return CC_EARCUT_UNKNOWN;
    size_t last = (size_t)-1, left = (size_t)-1;
    double cminx = F64_MAX;
    size_t cnt = end - start, pairs = cnt / 2;
    int fwd = clockwise == (signed_area(v, start, end) > 0.0);
    for (size_t k = 0; k < pairs; k++) {
        /* enumerate() indices are relative to `start`; the crate reads the box at them as is. */
        size_t xi = fwd ? 2 * k : cnt - 2 - 2 * k, yi = fwd ? 2 * k + 1 : cnt - 1 - 2 * k;
        double x = v[start + xi], y = v[start + yi];
        last = insert_node(ll, (start + xi) / 2, x, y, last);
        if (ll->err) return ll->err;
        if (cminx > x) {
            cminx = x;
            left = last;
        }
        if (ll->usehash) {
            ll->miny = fmin64(v[yi], ll->miny);
            ll->maxx = fmax64(v[xi], ll->maxx);
            ll->maxy = fmax64(v[yi], ll->maxy);
        }
    }
    ll->minx = fmin64(cminx, ll->minx);
    if (last == (size_t)-1 || left == (size_t)-1) return CC_EARCUT_UNKNOWN; /* Rust: unwrap panics */
    if (xy_eq(&N(last), &N(NEXT(last)))) {
        remove_node(ll, last);
        last = NEXT(last);
    }
    *last_out = last;
    *left_out = left;
    return 0;
}

/* filter_points; end = (size_t)-1 for None. */
static size_t filter_points(LL *ll, size_t start, size_t end) {
    if (end == (size_t)-1) end = start;
    if (end >= ll->len || start >= ll->len) return NUL;
    size_t p = start;
    for (;;) {
        if (!step(ll)) return NUL;
        int again = 0;
        if (!N(p).steiner && (xy_eq(&N(p), &N(NEXT(p))) || area(&N(PREV(p)), &N(p), &N(NEXT(p))) == 0.0)) {
            remove_node(ll, p);
            end = PREV(p);
            p = end;
            if (p == NEXT(p)) return end;
            again = 1;
        } else {
            if (p == NEXT(p)) return NUL;
            p = NEXT(p);
        }
        if (!again && p == end) return end;
    }
}

static int locally_inside(LL *ll, const Node *a, const Node *b) {
    const Node *pa = &N(a->prev), *na = &N(a->next);
    if (area(pa, a, na) < 0.0) return area(a, b, na) >= 0.0 && area(a, pa, b) >= 0.0;
    return area(a, b, pa) < 0.0 || area(a, na, b) < 0.0;
}

static int pseudo_intersects(const Node *p1, const Node *q1, const Node *p2, const Node *q2) {
    if ((xy_eq(p1, p2) && xy_eq(q1, q2)) || (xy_eq(p1, q2) && xy_eq(q1, p2))) return 1;
    return (area(p1, q1, p2) > 0.0) != (area(p1, q1, q2) > 0.0) &&
           (area(p2, q2, p1) > 0.0) != (area(p2, q2, q1) > 0.0);
}

static int intersects_polygon(LL *ll, const Node *a, const Node *b) {
    size_t start = (size_t)(a - ll->n), p = start; /* iter_pairs(a..a): the whole ring from a */
    do {
        const Node *pp = &N(p), *nn = &N(NEXT(p));
        if (pp->i != a->i && nn->i != a->i && pp->i != b->i && nn->i != b->i && pseudo_intersects(pp, nn, a, b))
            return 1;
        p = NEXT(p);
    } while (p != start);
    return 0;
}

static int middle_inside(LL *ll, const Node *a, const Node *b) {
    double two = 1.0 + 1.0;
    double mx = (a->x + b->x) / two, my = (a->y + b->y) / two;
    int inside = 0;
    size_t start = (size_t)(a - ll->n), p = start;
    do {
        const Node *pp = &N(p), *nn = &N(NEXT(p));
        if ((pp->y > my) != (nn->y > my) && nn->y != pp->y &&
            mx < (nn->x - pp->x) * (my - pp->y) / (nn->y - pp->y) + pp->x)
            inside = !inside;
        p = NEXT(p);
    } while (p != start);
    return inside;
}

static int is_valid_diagonal(LL *ll, size_t a, size_t b) {
    return N(NEXT(a)).i != N(b).i && N(PREV(a)).i != N(b).i && !intersects_polygon(ll, &N(a), &N(b)) &&
           locally_inside(ll, &N(a), &N(b)) && locally_inside(ll, &N(b), &N(a)) && middle_inside(ll, &N(a), &N(b));
}

/* split_bridge_polygon: returns the new node d, or 0 when out of memory. */
static size_t split_bridge_polygon(LL *ll, size_t a, size_t b) {
    if (!grow(ll, 2)) {
        ll->err = CC_EARCUT_OOM;
        return NUL;
    }
    size_t cidx = ll->len, didx = cidx + 1;
    Node c, d;
    memset(&c, 0, sizeof c);
    memset(&d, 0, sizeof d);
    c.i = N(a).i;
    c.x = N(a).x;
    c.y = N(a).y;
    d.i = N(b).i;
    d.x = N(b).x;
    d.y = N(b).y;
    size_t an = NEXT(a), bp = PREV(b);
    NEXT(a) = b;
    PREV(b) = a;
    c.next = an;
    PREV(an) = cidx;
    d.next = cidx;
    c.prev = didx;
    NEXT(bp) = didx;
    d.prev = bp;
    ll->n[ll->len++] = c;
    ll->n[ll->len++] = d;
    return didx;
}

/* find_hole_bridge (David Eberly) */
static size_t find_hole_bridge(LL *ll, size_t hole, size_t outer) {
    double hx = N(hole).x, hy = N(hole).y, qx = -1.0 / 0.0;
    size_t m = (size_t)-1, p = outer;
    do {
        const Node *pp = &N(p), *nn = &N(NEXT(p));
        if (hy <= pp->y && hy >= nn->y && nn->y != pp->y) {
            double cx = pp->x + (hy - pp->y) * (nn->x - pp->x) / (nn->y - pp->y);
            if (cx <= hx && qx < cx) {
                qx = cx;
                if (qx == hx && hy == pp->y) return p;
                if (qx == hx && hy == nn->y) return pp->next;
                m = pp->x < nn->x ? p : pp->next;
            }
        }
        p = NEXT(p);
    } while (p != outer);
    if (m == (size_t)-1) return NUL;
    if (hx == qx) return PREV(m);
    Node mp = N(m), n1, n2;
    memset(&n1, 0, sizeof n1);
    memset(&n2, 0, sizeof n2);
    n1.x = hy < mp.y ? hx : qx;
    n2.x = hy < mp.y ? qx : hx;
    n1.y = n2.y = hy;
    double tan_min = F64_MAX / 2.0;
    size_t best = m;
    p = NEXT(m);
    do {
        const Node *pp = &N(p);
        if (hx > pp->x && pp->x >= mp.x && contains(&n1, &mp, &n2, pp)) {
            double t = fabs64(hy - pp->y) / (hx - pp->x);
            if ((t < tan_min || (t == tan_min && pp->x > N(best).x)) && locally_inside(ll, pp, &N(hole))) {
                best = p;
                tan_min = fabs64(hy - pp->y) / (hx - pp->x);
            }
        }
        p = NEXT(p);
    } while (p != m);
    return best;
}

static void eliminate_hole(LL *ll, size_t hole, size_t outer) {
    size_t test = find_hole_bridge(ll, hole, outer);
    size_t b = split_bridge_polygon(ll, test, hole);
    if (ll->err) return;
    filter_points(ll, b, NEXT(b));
}

typedef struct {
    size_t idx;
    double x;
} Hole;

static int eliminate_holes(LL *ll, const double *v, size_t vlen, const size_t *holes, size_t nholes, size_t outer,
                           size_t *out) {
    Hole *q = malloc((nholes ? nholes : 1) * 2 * sizeof(Hole));
    if (!q) return CC_EARCUT_OOM;
    for (size_t k = 0; k < nholes; k++) {
        if (holes[k] > ((size_t)-1) / 2 || (k + 1 < nholes && holes[k + 1] > ((size_t)-1) / 2)) {
            free(q);
            return CC_EARCUT_UNKNOWN;
        }
        size_t s = holes[k] * 2, e = k + 1 < nholes ? holes[k + 1] * 2 : vlen, list, left;
        int rc = add_contour(ll, v, vlen, s, e, 0, &list, &left);
        if (rc) {
            free(q);
            return rc;
        }
        if (list == NEXT(list)) N(list).steiner = 1;
        q[k].idx = left;
        q[k].x = N(left).x;
    }
    /* sort_by partial_cmp of x (NaN: Equal), stable: bottom-up merge sort */
    Hole *tmp = q + nholes;
    for (size_t w = 1; w < nholes; w *= 2) {
        for (size_t lo = 0; lo < nholes; lo += 2 * w) {
            size_t mid = lo + w < nholes ? lo + w : nholes, hi = lo + 2 * w < nholes ? lo + 2 * w : nholes;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = q[j].x < q[i].x ? q[j++] : q[i++];
            while (i < mid) tmp[k++] = q[i++];
            while (j < hi) tmp[k++] = q[j++];
        }
        memcpy(q, tmp, nholes * sizeof(Hole));
    }
    for (size_t k = 0; k < nholes; k++) {
        eliminate_hole(ll, q[k].idx, outer);
        if (ll->err) break;
        outer = filter_points(ll, outer, NEXT(outer));
    }
    free(q);
    *out = outer;
    return ll->err;
}

static int is_ear(LL *ll, size_t prev, size_t ear, size_t next) {
    const Node *a = &N(prev), *b = &N(ear), *c = &N(next);
    if (area(a, b, c) >= 0.0) return 0;
    size_t p = NEXT(next);
    do {
        const Node *pp = &N(p);
        if (contains(a, b, c, pp) && area(&N(PREV(p)), pp, &N(NEXT(p))) >= 0.0) return 0;
        p = NEXT(p);
    } while (p != prev);
    return 1;
}

static int earcheck(LL *ll, const Node *a, const Node *b, const Node *c, size_t p) {
    const Node *pp = &N(p);
    return p != (size_t)(a - ll->n) && p != (size_t)(c - ll->n) && contains(a, b, c, pp) &&
           area(&N(PREV(p)), pp, &N(NEXT(p))) >= 0.0;
}

/* NodeTriangle::is_ear_hashed: 1 ear, 0 not, -1 Error::Unknown. prev/ear/next are copies in
 * Rust; only node 0's z changes here, so the nodes themselves can stand in for them. */
static int is_ear_hashed(LL *ll, size_t ip, size_t ie, size_t in) {
    Node prev = N(ip), ear = N(ie), next = N(in);
    if (area(&prev, &ear, &next) >= 0.0) return 0;
    double maxx = fmax64(prev.x, fmax64(ear.x, next.x)), maxy = fmax64(prev.y, fmax64(ear.y, next.y));
    double minx = fmin64(prev.x, fmin64(ear.x, next.x)), miny = fmin64(prev.y, fmin64(ear.y, next.y));
    int32_t min_z, max_z;
    if (zorder(minx, miny, ll->invsize, &min_z) || zorder(maxx, maxy, ll->invsize, &max_z)) return -1;
    const Node *a = &N(ip), *b = &N(ie), *c = &N(in);
    size_t p = ear.prevz, n = ear.nextz;
    while (p != NUL && N(p).z >= min_z && n != NUL && N(n).z <= max_z) {
        if (!step(ll)) return 0;
        if (earcheck(ll, a, b, c, p)) return 0;
        p = N(p).prevz;
        if (earcheck(ll, a, b, c, n)) return 0;
        n = N(n).nextz;
    }
    N(NUL).z = (int32_t)((uint32_t)min_z - 1u);
    while (N(p).z >= min_z) {
        if (!step(ll)) return 0;
        if (earcheck(ll, a, b, c, p)) return 0;
        p = N(p).prevz;
    }
    N(NUL).z = (int32_t)((uint32_t)max_z + 1u);
    while (N(n).z <= max_z) {
        if (!step(ll)) return 0;
        if (earcheck(ll, a, b, c, n)) return 0;
        n = N(n).nextz;
    }
    return 1;
}

static int earcut_linked(LL *ll, size_t ear, int pass, int hashed, int depth);

/* cure_local_intersections */
static size_t cure_local_intersections(LL *ll, size_t instart) {
    size_t p = instart, start = instart;
    for (;;) {
        if (!step(ll)) return p;
        size_t a = PREV(p), b = NEXT(NEXT(p));
        if (!xy_eq(&N(a), &N(b)) && pseudo_intersects(&N(a), &N(p), &N(NEXT(p)), &N(b)) &&
            locally_inside(ll, &N(a), &N(b)) && locally_inside(ll, &N(b), &N(a))) {
            push_tri(ll, N(a).i, N(p).i, N(b).i);
            remove_node(ll, p);
            size_t nidx = NEXT(p);
            remove_node(ll, nidx);
            start = b;
            p = start;
        }
        p = NEXT(p);
        if (p == start) break;
    }
    return p;
}

/* split_earcut: the halves always go through the hashed path, as in the crate. */
static int split_earcut(LL *ll, size_t start, int depth) {
    size_t a = start;
    do {
        size_t b = NEXT(NEXT(a));
        while (b != PREV(a)) {
            if (!step(ll)) return ll->err;
            if (N(a).i != N(b).i && is_valid_diagonal(ll, a, b)) {
                size_t c = split_bridge_polygon(ll, a, b);
                if (ll->err) return ll->err;
                size_t an = NEXT(a), cn = NEXT(c);
                a = filter_points(ll, a, an);
                c = filter_points(ll, c, cn);
                int rc = earcut_linked(ll, a, 0, 1, depth + 1);
                if (rc) return rc;
                return earcut_linked(ll, c, 0, 1, depth + 1);
            }
            b = NEXT(b);
        }
        a = NEXT(a);
    } while (a != start);
    return 0;
}

/* earcut_linked_hashed / earcut_linked_unhashed, PASS = pass. */
static int earcut_linked(LL *ll, size_t ear, int pass, int hashed, int depth) {
    if (depth > MAX_DEPTH) return ll->err = CC_EARCUT_LIMIT;
    if (ll->err) return ll->err;
    if (hashed && pass == 0 && index_curve(ll, ear)) return CC_EARCUT_UNKNOWN;
    size_t stop = ear, prev = 0, next = NEXT(ear);
    while (stop != next) {
        if (!step(ll)) return ll->err;
        prev = PREV(ear);
        next = NEXT(ear);
        int is;
        if (hashed) {
            is = is_ear_hashed(ll, prev, ear, next);
            if (is < 0) return CC_EARCUT_UNKNOWN;
        } else {
            is = is_ear(ll, prev, ear, next);
        }
        if (ll->err) return ll->err;
        if (is) {
            push_tri(ll, N(prev).i, N(ear).i, N(next).i);
            remove_node(ll, ear);
            ear = NEXT(next);
            stop = ear;
        } else {
            ear = next;
        }
    }
    if (ll->err) return ll->err;
    if (prev == next) return 0;
    if (pass == 0) {
        size_t tmp = filter_points(ll, next, (size_t)-1);
        if (ll->err) return ll->err;
        return earcut_linked(ll, tmp, 1, hashed, depth + 1);
    }
    if (pass == 1) {
        size_t e = cure_local_intersections(ll, next);
        if (ll->err) return ll->err;
        return earcut_linked(ll, e, 2, hashed, depth + 1);
    }
    return split_earcut(ll, next, depth + 1);
}

int cc_earcut(const double *v, size_t nv, const size_t *holes, size_t nholes, uint32_t **tri, size_t *ntri) {
    *tri = NULL;
    *ntri = 0;
    if (nv == 0 && nholes == 0) return 0;
    if (nv % 2 == 1 || 2 > nv) return CC_EARCUT_UNKNOWN;
    size_t outer_len = nv;
    if (nholes) {
        if (holes[0] > ((size_t)-1) / 2) return CC_EARCUT_UNKNOWN;
        outer_len = holes[0] * 2;
        if (outer_len > nv || outer_len == 0) return CC_EARCUT_UNKNOWN;
    }
    LL ll;
    memset(&ll, 0, sizeof ll);
    ll.minx = ll.miny = F64_MAX;
    ll.maxx = ll.maxy = -F64_MAX;
    ll.usehash = nv >= 80;
    int rc = CC_EARCUT_OOM;
    if (!grow(&ll, nv / 2 + 1)) goto done;
    memset(&ll.n[0], 0, sizeof(Node)); /* the NULL node */
    ll.len = 1;
    size_t outer, left;
    rc = add_contour(&ll, v, nv, 0, outer_len, 1, &outer, &left);
    if (rc) goto done;
    rc = eliminate_holes(&ll, v, nv, holes, nholes, outer, &outer);
    if (rc) goto done;
    if (ll.usehash) {
        double d = fmax64(ll.maxx - ll.minx, ll.maxy - ll.miny);
        ll.invsize = d == 0.0 ? 0.0 : 32767.0 / d;
        double mx = ll.minx, my = ll.miny;
        for (size_t k = 0; k < ll.len; k++) {
            ll.n[k].x = ll.n[k].x - mx;
            ll.n[k].y = ll.n[k].y - my;
        }
    }
    rc = earcut_linked(&ll, outer, 0, ll.usehash, 0);
    if (!rc) rc = ll.err;
done:
    free(ll.n);
    if (rc) {
        free(ll.tri);
        return rc;
    }
    *tri = ll.tri;
    *ntri = ll.ntri;
    return 0;
}
