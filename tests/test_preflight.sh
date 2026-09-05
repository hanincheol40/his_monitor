#!/bin/bash
# ============================================================================
#  test_preflight.sh — black-box tests for the pre-run input checker.
#
#  Black box on purpose. preflight's dangerous failure is not a crash, it is
#  printing "OK" for a file that is not OK, and that is a property of the whole
#  program -- parser, checks and reporting together. Testing the parser
#  functions in isolation would not have caught the one real bug here (the
#  intercept being read from the wrong parenthesis), because that bug produced
#  a perfectly well-formed number.
#
#  Run:  make test        (or  bash tests/test_preflight.sh ./preflight )
# ============================================================================
set -u
PF=${1:-./preflight}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0

check () {   # check <name> <expected-substring> <file...>
    local name=$1 want=$2; shift 2
    local got
    got=$("$PF" "$@" -v 2>&1 | sed 's/\x1b\[[0-9;]*m//g')
    if printf '%s' "$got" | grep -q -- "$want"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf '  FAIL %s\n       want to see: %s\n       got: %s\n' \
               "$name" "$want" "$(printf '%s' "$got" | head -2 | tr '\n' ' ')"
    fi
}

check_exit () {   # check_exit <name> <expected-code> <args...>
    local name=$1 want=$2; shift 2
    "$PF" "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" = "$want" ]; then pass=$((pass+1))
    else fail=$((fail+1)); printf '  FAIL %s: exit %s, want %s\n' "$name" "$got" "$want"; fi
}

# --- builders ---------------------------------------------------------------
# One domain, two elements. $1 = file, $2 = the radius expression, $3 = dt
mk () {
    local f=$1 r=$2 dt=${3:-1.000e-05}
    cat > "$f" <<END
12 	  parameter list
0 	 EQTYPE
$dt 	 DT
6.500e+05 	 NSTEPS
100 	 HISSTEP
1060.0 	 Rho
9.986e+03 	 Pext
Mesh -- expansion order --  quadrature order Ndomains = 1
2     nel domain  1 Eh Area Gamma
0.0000 0.0200 3 3 # x_prox x_dist p q
Area = PI* ($r) * ($r)
Eh =  0.1*(3000000*exp(-13.5*100*($r)) +538490.4467)*($r)
0.0200 0.0400 3 3 # x_prox x_dist p q
Area = PI* ($r) * ($r)
Eh =  0.1*(3000000*exp(-13.5*100*($r)) +538490.4467)*($r)
Boundary conditions
u 0.0
a 0.0
W 1.0e9
W 2.0e10
END
}

echo "test_preflight"

# --- the normal case must still pass ---------------------------------------
mk "$TMP/good.in" "-0.0033667*x+0.018206"
check "a well-formed input is OK"              "OK"    "$TMP/good.in"

# A constant radius has no sign inside the factor after the first number
# except the '+'; domain 2 of the real model looks exactly like this.
mk "$TMP/const.in" "0*x+0.015799"
check "constant-radius formula parses"          "OK"    "$TMP/const.in"

# --- radius crossing zero must be FATAL ------------------------------------
# slope -0.5 over a 0.04 m domain takes r from 0.01 to -0.01
mk "$TMP/cross.in" "-0.5*x+0.01"
check "radius crossing zero is FATAL"           "FATAL" "$TMP/cross.in"

# --- the regression this test exists for -----------------------------------
# A negative intercept leaves no '+' in the first factor. Searching the whole
# line finds the '+' in a later term and yields icept = 0.5 -- a half-metre
# radius, which then passes every check and the file is reported OK.
cat > "$TMP/neg.in" <<'END'
12 	  parameter list
1.000e-05 	 DT
1060.0 	 Rho
9.986e+03 	 Pext
Mesh -- expansion order --  quadrature order Ndomains = 1
1     nel domain  1 Eh Area Gamma
0.0000 0.0200 3 3 # x_prox x_dist p q
Area = PI* (-0.0033667*x-0.018206) * (1.0+0.5)
Eh =  0.1*(3000000*exp(-13.5*100*(-0.0033667*x-0.018206)) +538490.4467)*(-0.0033667*x-0.018206)
END
if "$PF" "$TMP/neg.in" -v 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -q "OK"; then
    fail=$((fail+1))
    echo "  FAIL a negative-intercept formula must not be reported OK"
else
    pass=$((pass+1))
fi

# --- CFL --------------------------------------------------------------------
# Same geometry, a time step 200x larger. dt*c/dx must go over the limit.
mk "$TMP/cfl.in" "-0.0033667*x+0.018206" "2.000e-03"
check "an oversized time step is RISK"          "RISK"  "$TMP/cfl.in"

# --- files that are not inputs ---------------------------------------------
printf 'this is not a Nektar1D input at all\n' > "$TMP/junk.in"
check "a non-input file is UNREADABLE"          "UNREADABLE" "$TMP/junk.in"

printf 'Mesh Ndomains = 0\n' > "$TMP/zero.in"
check "Ndomains = 0 is refused"                 "UNREADABLE" "$TMP/zero.in"

# --- the reference must be usable ------------------------------------------
# read_in() succeeds on any file with an Ndomains line, so a reference whose
# geometry does not parse would leave every comparison silently skipped and
# the whole cohort would be reported clean having been compared against
# nothing. That must be refused, not passed.
cat > "$TMP/badref.in" <<'END'
1.000e-05 	 DT
1060.0 	 Rho
Mesh -- expansion order --  quadrature order Ndomains = 1
1     nel domain  1 Eh Area Gamma
0.0000 0.0200 3 3 # x_prox x_dist p q
Area = something the parser does not understand
END
check_exit "an unparseable --ref is refused"    1  "$TMP/good.in" --ref "$TMP/badref.in"
if "$PF" "$TMP/good.in" --ref "$TMP/badref.in" 2>&1 | grep -q "no parseable geometry"; then
    pass=$((pass+1))
else
    fail=$((fail+1)); echo "  FAIL refusing a bad --ref must say why"
fi

# --- exit codes -------------------------------------------------------------
check_exit "clean input exits 0"                0  "$TMP/good.in"
check_exit "flagged input exits 1"              1  "$TMP/cross.in"
check_exit "no arguments exits 1"               1
check_exit "a glob matching nothing exits 1"    1  --ref "$TMP/good.in"

echo "$((pass+fail)) checks, $fail failed"
[ "$fail" = 0 ]
