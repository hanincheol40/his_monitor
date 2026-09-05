#!/bin/sh
# Generate synthetic .his files so the monitor itself can be run under the
# sanitizers without the solver.
#
# This exists because the interesting concurrency lives in main.c -- the reader
# pool, the tick barrier, the generation counter -- and none of it is linked
# into tests/test_hisfile, which drives the parsing layer directly. Running the
# unit tests under ThreadSanitizer therefore proves nothing about the pool: the
# process never creates a thread. CI generates a tree with this and runs the
# real binary against it.
#
# usage: make_his.sh <dir> <base> <ndomains> <nrows>
set -e

dir=${1:?usage: make_his.sh <dir> <base> <ndomains> <nrows>}
base=${2:?}
ndom=${3:?}
nrow=${4:?}

mkdir -p "$dir"

# The monitor learns the domain count, the time step and the history-point
# layout from the .in file, so there has to be one. Only the handful of fields
# in_mesh() and in_param() actually read are written here.
{
    echo "1.000e-05 	 DT"
    echo "100 	 HISSTEP"
    echo "$((nrow * 100)) 	 NSTEPS"
    echo "Mesh -- expansion order --  quadrature order Ndomains = $ndom"
    i=1
    while [ "$i" -le "$ndom" ]; do
        echo "1     nel domain  $i Eh Area Gamma"
        echo "0.0000 0.0400 3 3 # x_prox x_dist p q"
        i=$((i + 1))
    done
    echo "History Pts"
    echo "$ndom  #Number of Domains with history Points"
    i=1
    while [ "$i" -le "$ndom" ]; do
        echo "2 $i"
        echo "0.0000 0.0400"
        i=$((i + 1))
    done
} > "$dir/$base.in"

# Two history points per file, matching what the solver writes: the monitor
# watches the distal one by default and has to skip the other, so a file with
# only one point would leave that filter untested.
i=1
while [ "$i" -le "$ndom" ]; do
    awk -v n="$nrow" -v seed="$i" '
    BEGIN {
        pi = 3.14159265358979
        T  = 0.8                       # one cardiac cycle, seconds
        dt = 0.001                     # 1 kHz, as HISSTEP=100 at DT=1e-5 gives
        # a small per-domain offset so the domains are not bit-identical and
        # the wave appears to arrive at different times down the tree
        lag = (seed % 40) * 0.004

        print "# synthetic history file - tests/make_his.sh, no solver involved"
        print "# History points: 2"
        print "# t, P(x,t), Pe(x,t), U(x,t), Q(x,t), A(x,t), # point"

        for (k = 0; k < n; k++) {
            t = k * dt
            p = (t - lag) / T
            p = p - int(p)             # fractional part of the cycle
            if (p < 0) p += 1

            # sharp systolic upstroke, then an exponential diastolic decay:
            # enough of a foot for the detector and the cycle counter to work
            if (p < 0.30) {
                s = sin(pi * p / 0.30)
                P = 10000 + 3300 * s * s
            } else {
                P = 10000 + 1150 * exp(-(p - 0.30) * 2.6)
            }
            U = 0.05 + 0.9 * (P - 10000) / 3300
            A = 3.1e-4
            Q = U * A

            for (pt = 1; pt <= 2; pt++)
                printf "%.6f %.4f %.4f %.6f %.9f %.9f %d\n", \
                       t, P, 0.0, U, Q, A, pt
        }
    }' > "$dir/${base}_$i.his"
    i=$((i + 1))
done

echo "make_his.sh: $ndom files, $nrow rows each, in $dir"
