# his_monitor

A live field monitor for a running 1-D blood flow simulation, and a pre-run
input checker to go with it.

![the monitor attached to a running solver](docs/screenshot.png)

A 1-D haemodynamic solver integrates a 116-artery arterial tree and writes one
time-series file per arterial segment as it goes. A single subject is a few
hundred thousand time steps — minutes of wall clock. A generated cohort is
thousands of subjects, which is days.

The problem was never the runtime. It was that a wrong parameter only showed up
after the whole cohort had finished and the post-processing had run. So a
mis-scaled radius cost days before anybody saw it.

The solver was already writing everything needed, every millisecond, to disk.
This reads it while the run is still going.

Two programs:

| binary | what it does |
|---|---|
| `his_monitor` | attaches to a running solver and draws the 116-artery pressure field, judging convergence, divergence and stall as it goes |
| `preflight` | reads input files only, and flags ones that differ from a configuration known to run, **before** anything is started |

---

## Build

```sh
make            # -> his_monitor, preflight
make test       # -> 66 + 13 checks, all from bugs that actually happened
```

C11, libm and pthreads. Nothing else, no X11: it works over a plain ssh
session. Built and run on Ubuntu 22.04 / GCC 11.4, clean under
`-Wall -Wextra -pedantic`.

> `CFLAGS` carries `-D_POSIX_C_SOURCE=200809L`. Without it `-std=c11` hides
> `strtok_r`, `clock_nanosleep` and `TIOCGWINSZ` from the standard headers; the
> compiler falls back to an implicit `int`-returning declaration, the returned
> pointer is truncated to 32 bits, and the program segfaults at run time while
> still compiling cleanly. If you override `CFLAGS`, keep that define.

---

## Use

Two terminals. The solver does not know the monitor exists, so attaching or
detaching has no effect on the run.

```sh
# terminal 1: the solver, writing sim_1_1.his .. sim_1_116.his
# terminal 2, same directory:
./his_monitor sim_1 --names 116_artery_model.txt
```

Each cell is one arterial segment: number, name, current pressure, a `>` when
that vessel is on the steep part of its own upstroke, and how many
milliseconds after the aortic root the wave foot arrived there. Colour is the
classic rainbow map, so the picture matches the figures the 1-D community
already uses.

In the screenshot above, taken from a live run: the aorta and head sit near
100 mmHg, the arms are up at 117–128 where the reflected wave has arrived, and
the tibials are still at 67 because the wave has not reached them. Arrival
times run 0 → 54–85 (arms) → 134 (femoral) → 163–168 ms (ankle).

`wall` is how long the *monitor* has been running, not the solver — here it
attached 270 s into the run. `eta` is derived from how fast simulated time
advances while the monitor watches, which is what makes it right when you
attach partway through.

### Options

```
--names <file>       segment-number to artery-name table, to label the cells
--from-start         read the files from the beginning instead of following
                     only what is appended from now on
--proximal           watch the proximal history point, not the distal one
--period <ms>        display period, default 250
--threads <n>        reader threads, default min(8, cores)
--pwv-target <m/s>   expected carotid-femoral PWV for this subject
--tol <pct>          how far cf-PWV may drift before it is a failure (10)
--umax <m/s>         peak |U| above which the run is called off target
                     (default 2.0; 0 disables the check)
--stall <s>          seconds without new samples before calling it stalled (20)
--abort-on-fail      exit 2 once the verdict has been OFF TARGET or STALLED for
                     three display periods in a row
--bench              time a full read: warm-up, 1 thread, N threads, 1 thread
                     again. Prints the four times and exits
```

`COLUMNS` and `LINES` are honoured when there is no terminal to ask, which is
what makes the layout testable without a tty.

### Verdicts

Checked in this order; the first that matches wins.

| verdict | meaning |
|---|---|
| `STALLED` | no new samples anywhere for `--stall` seconds |
| `OFF TARGET` | some domain is writing nan/inf — the solution diverged |
| `STALLED` | *some* domains are individually quiet while others stream |
| `FILLING` | fewer than two cycles seen; the field is still growing |
| `OFF TARGET` | peak \|U\| over the limit, naming the domain |
| `OFF TARGET` | settled, but cf-PWV is outside tolerance of `--pwv-target` |
| `CONVERGING` | cycle-to-cycle systolic change above 0.5 mmHg |
| `CONVERGED` | below 0.5 mmHg — the solution is periodic |

The exit code is 2 for `OFF TARGET` or `STALLED`, so a launch script can act on
it. `OFF TARGET` on cf-PWV is only ever raised **after** the solution has
settled — judging it during the filling transient kills perfectly good runs,
which it did, at cycle 3, before that guard existed.

---

## Five things worth knowing about the code

**Tailing a file somebody else is writing.** The last line is routinely half
written. Reading it yields a truncated row that parses as perfectly valid
numbers, which silently corrupts the series. The file position is saved before
each read and restored when the line does not end in a newline. `clearerr()` is
also needed — once `fgets` has seen EOF the stream latches it and every later
read returns nothing even after the file has grown — but `ferror` has to be
checked first, or a genuine read error looks exactly like "nothing new yet" and
the domain polls a dead stream in silence.

**The column header carries commas of its own.** Column names come with
argument lists, `P(x,t)`, and the fields are comma-separated. Splitting on
commas cuts that into `P(x` and `t)`, turning seven columns into twelve and
putting every later index on the wrong number. The parenthesised parts are
stripped before the split. The column set also varies with the options a run
was launched with, so it is read from the file rather than assumed.

**The default mode has to read the header before skipping to the end.** The
layout is declared once, at the top of the file, and every data row is
discarded until it has been parsed. Following a file by seeking straight to its
end therefore reads the whole run and accepts nothing — while still counting
bytes, so it looks busy — and then reports "no samples, wrong base name",
blaming a correct command. That was the *default* path, and it survived a long
time because every screenshot, every benchmark and every test used
`--from-start`, which passes the header on the way through.

**The foot detector leaves its state on a timer, not a level.** Holding a
running minimum and firing when the pressure climbs above it is the easy half.
Coming back out is the trap: the obvious condition — the pressure returns to
the diastole just recorded — latches on cycle 1, because cycle 1's diastole is
the t=0 startup value and the solution never goes back there.

**Two feet have to come from the same heartbeat.** The wave reaches the carotid
about 17 ms after the root and the femoral about 134 ms after, so for roughly a
sixth of every cycle one holds this beat's foot and the other still holds the
previous one. Subtracting those gives a negative transit time. The same trap
appears twice: in cf-PWV, where it collapsed to 0 on 30 of 161 live frames and
made the verdict skip the PWV check entirely; and in the arrival time printed
in each cell, which read −656 ms at the ankle. Both now pair the feet by
looking for a difference in a plausible transit window. Pairing by cycle
*number* is the obvious fix and is wrong — the counters drift apart over a run,
and that version returned 0 on 180 frames out of 180.

None of these show up against synthetic data, and the last two do not show up
against a finished file either — only against a solver that is still running.

---

## preflight

Better than catching a divergence three minutes in is not starting the run.

```sh
./preflight sim_*.in --ref <an-input-that-runs>.in
```

Checks, per domain, from the geometry formulas the input carries: the radius
stays positive over the whole domain; the initial area is positive once the
external pressure has deflated it; `CFL = dt·c/dx` is small enough for explicit
stepping, with `c` from the same empirical wall law the model itself uses; and,
with `--ref`, how far the radii have drifted from an input known to run.

### How the radius threshold was set

By experiment. Starting from an input that runs to completion, every radius was
scaled by a constant with the terminal boundary conditions left untouched, and
each result was actually run:

| scale | ×0.90 | ×0.80 | ×0.78 | ×0.76 | ×0.75 | ×0.74 | ×0.72 | ×0.70 | ×0.60 | ×0.50 |
|---|---|---|---|---|---|---|---|---|---|---|
| solver | runs | runs | runs | runs | **runs** | **dies** | dies | dies | dies | dies |

The boundary is at about a 25% radius reduction, and it is sharp. The default
`--rtol` of 15% therefore flags earlier than strictly necessary: on that sweep
it calls ×0.80 through ×0.75 a risk although they run. Stated plainly, on this
set: **5 of 5 failures caught, 4 of 5 working inputs flagged anyway.** That
asymmetry is deliberate — a false positive costs one look at a file, a false
negative costs a share of a multi-day cohort.

### What it does not do

`preflight` does **not** predict divergence from first principles. Run without
`--ref` it reports `OK` for an input that dies in the first time step: positive
radii, positive initial area, CFL 0.174 — it passes every first-principles
check there is here. It only catches that input when given a working
configuration to compare against, and the reference has to come from the same
family: comparing a cohort against an unrelated model flags all of it.

The terminal impedance `R/Z0` is reported but **not** used to fail a run,
because it does not separate good inputs from bad ones here. The highest value
measured anywhere belongs to an input that runs to completion, and the one that
reliably dies is mid-range. There is no threshold that works, so there is no
threshold — just the number, and `--zlim` for anyone with evidence to set one.

---

## Verified

| | |
|---|---|
| build | `-O2 -std=c11 -Wall -Wextra -pedantic`, zero warnings; cppcheck clean |
| tests | `make test` — 66 checks on the parsing/analysis/verdict layers, 13 on the checker end to end |
| dynamic | ThreadSanitizer 0 races (live monitoring at 8 threads, and `--bench`); ASan + UBSan 0 errors, including the test suite |
| parallel read | 147.7 MB across 116 files: 1 thread 1.01 s → 8 threads 0.16 s, **about 6.4x** (6.3–6.5 across runs) |
| coverage | all 116 domains report a value in the final frame, at 1, 2 and 8 reader threads alike |

The benchmark runs four passes — a discarded warm-up, then 1 thread, N threads,
and 1 thread again — because measurement order alone changes the answer. The
first version measured 1 then N and reported 7.25x; the warm-up pass, which
puts both timed passes on the same page cache, brought it to 6.4x. The trailing
single-threaded pass says whether anything drifted between the two.

## Not verified

- The `--umax` check is **unexercised**. Every failure reproducible here dies in
  the first time step, so the gradual velocity rise it exists to catch never
  occurs. It is a reasoned design, not a validated one.
- cf-PWV has **no end-to-end validation** against an independent reference. The
  wall-law formula was checked against published values to within 3%, but that
  validates the formula, not this tool's foot detection and path definition.
- No RTOS. The periodic-task discipline here is ordinary Linux with no hard
  real-time guarantee — an absolute-deadline `clock_nanosleep` on
  `CLOCK_MONOTONIC`, with jitter and overruns counted and displayed.
- This was written after the fact. It has never been used to shorten a real
  cohort run.

---

## Scope

This repository contains only my own code. The 1-D solver whose output it reads
is Nektar1D, © King's College London, licensed to me for non-commercial
research use under terms that do not permit redistribution. It is **not**
included here — no source, no inputs, no outputs from it, and nothing in this
repository reproduces any part of it. Everything here works from the file
formats as observed at run time.

The 116-artery arterial model and the pulse wave database it comes from are
published open access by Charlton et al.; the artery name table the `--names`
option reads is part of that public release and is likewise not bundled here.

## Licence

MIT — see `LICENSE`.
