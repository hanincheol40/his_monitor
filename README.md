<div align="center">

# his_monitor

**Watch a 1-D blood flow simulation while it runs, and screen its inputs before it starts.**

A C monitor with two views (a terminal display that needs nothing but ssh, and a Qt
window), plus a pre-run input checker.

[![ci](https://github.com/hanincheol40/his_monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/hanincheol40/his_monitor/actions/workflows/ci.yml)
![C11](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)
![platform](https://img.shields.io/badge/platform-Linux-lightgrey?logo=linux&logoColor=white)
![license](https://img.shields.io/badge/license-MIT-green)

[한국어 설명](README.ko.md)

</div>

![the Qt window on a live run](docs/gui.png)

<div align="center"><sub>
The window on a real run, 5.0 s of simulated time in and still converging. Each cell is one
vessel, coloured by its pressure right now: a new beat has reached the head and neck (dark)
and not yet the arms and legs (light). On the right, the aortic root's pressure over the
last two seconds, and how much the systolic pressure changed from each beat to the next. The
change halves every beat, heading for the 0.5 mmHg line.
</sub></div>

---

## The problem

A simulation of the human arterial tree runs for minutes per patient. A study is thousands
of patients, so it runs for days on a remote machine, with no screen, started over ssh and
left alone.

The runtime was never the problem. The problem was that one wrong number in an input file
only showed up after all of it had finished. A mis-scaled vessel radius cost days of compute
before anybody saw it.

The solver was already writing everything needed, every millisecond, to disk. Nobody was
reading it until the end.

| binary | what it does |
|---|---|
| `his_monitor` | attaches to a run that is already going and draws all 116 vessels live, judging convergence, divergence and stall as it goes. Exits 2 on a bad verdict, so a launch script can kill the run |
| `his_gui` | a Qt window on the same monitor: the field of vessels, the waveform of any one of them, and convergence beat by beat, for a run on this machine or on a server over ssh. It draws; the monitor decides |
| `preflight` | reads input files only, and flags ones that differ from a configuration known to work, before anything is started |

None of them touches the solver. Attaching or detaching the monitor has no effect on the
run, because all it does is read files.

## At a glance

| | |
|---|---|
| Language | C11 for the monitor and the checker: libm and pthreads only, nothing to install. C++17 with Qt 5 Widgets for the optional window |
| Size | 2,405 lines of C, 1,664 lines of C++ for the window, 1,107 lines of tests and test fixtures |
| Concurrency | worker pool over a shared work queue, released and rejoined once per display period; 6.3× read speedup on 8 threads (147.7 MB across 116 files: 1.01 s → 0.16 s) |
| Scheduling | periodic display task on absolute deadlines (`clock_nanosleep`, `CLOCK_MONOTONIC`), with missed deadlines and worst-case lateness counted and shown on screen |
| Benchmark | the first version reported 7.25×; adding a discarded warm-up pass, so both timed passes see the same page cache, brought it to 6.3× |
| Tests | 108 checks (83 unit + 13 checker end-to-end + 12 stream end-to-end). Every one was written from a bug that actually happened |
| Checked in CI | `-Werror` build, the test suite, then the real monitor binary run against generated data under ThreadSanitizer, AddressSanitizer and UBSan, plus cppcheck. The window is built with `-Werror`, its parser is run over real monitor output, and it is run headless against the monitor |
| Runs on | Ubuntu 22.04 / GCC 11.4, over ssh, no display needed |

---

## Build and run

```sh
make            # -> his_monitor, preflight
make test       # -> 108 checks
make gui        # -> gui/build/his_gui    (needs Qt 5 and CMake; the rest does not)
```

Two terminals, same directory:

```sh
# terminal 1: the solver, writing sim_1_1.his .. sim_1_116.his
# terminal 2, either view:
./his_monitor sim_1 --names 116_artery_model.txt              # in the terminal
./gui/build/his_gui sim_1 --names 116_artery_model.txt        # in a window
```

The run is usually on a server. The window can stay on your own machine and start the
monitor over there. It needs key-based ssh, and nothing on the server but `his_monitor`:

```sh
./gui/build/his_gui sim_1 --ssh lab-server --dir /path/to/run --names 116_artery_model.txt
```

### Try it without the solver

The solver is not in this repository, so there is a generator for synthetic
history files. It writes a 116-file tree with a plausible pressure waveform and
two history points per file. That is enough to exercise both binaries, and it is
what the sanitizer jobs in CI run against.

```sh
sh tests/make_his.sh /tmp/demo synth 116 3000
./his_monitor /tmp/demo/synth --from-start
./gui/build/his_gui /tmp/demo/synth --from-start      # or the window
```

Three seconds of simulated time, about four heartbeats: the field fills, the
verdict settles on `CONVERGED`, and the status line shows the reader pool and the
tick timing doing real work. Ctrl-C (or close the window) to quit.

> [!IMPORTANT]
> `-D_POSIX_C_SOURCE=200809L` is required, and every source file defines it for itself
> before its first include; the `CFLAGS` entry is there so a newly added file inherits it
> too. Without it, `-std=c11` hides `strtok_r`, `clock_nanosleep` and `TIOCGWINSZ` from the
> standard headers. Two of those three fail loudly: `clock_nanosleep` returns `int`, and
> `TIOCGWINSZ` is a macro, so hiding it is a compile error. `strtok_r` is the dangerous
> one. An implicit `int`-returning declaration truncates the returned `char *` to 32 bits
> and the program crashes at run time. `-Wall` does warn about it, and CI promotes that
> warning to an error, so this only bites if the warning is ignored or `-Wall` is dropped
> as well. It was ignored once, which is why this note exists.

---

## How it works

### Where the code lives

| file | lines | what is in it |
|---|---|---|
| `src/main.c` | 454 | CLI, the reader pool, the periodic display loop, the ways out |
| `src/hisfile.c` | 429 | reading the input file and tailing the output files |
| `src/wave.c` | 485 | pulse foot detection, wave speed, dSBP, the verdict |
| `src/render.c` | 219 | the terminal view |
| `src/stream.c` | 172 | the same view as JSON lines, for the window |
| `src/preflight.c` | 458 | the standalone input checker |
| `src/*.h` | 188 | shared types |
| `gui/src/` | 1,664 | the Qt window: stream client, vessel field, waveform, convergence |

### The reader pool

116 files have to be read every display period, and they are wildly uneven in size. Threads
therefore do not get a fixed slice of the tree. They share one queue and take the next
index off it under a mutex, so a thread that draws a small file comes back for another
instead of finishing early and idling.

The main thread publishes a tick by bumping a generation counter and broadcasting; each
worker wakes, drains the queue, and decrements an active count on the way out. The last one
out signals the main thread, which is blocked until every domain in this tick has been
read. The generation counter is what makes the wakeup safe: without it, a worker that was
still finishing the previous tick when the broadcast went out would miss it and the tick
would never complete.

Two details worth naming, both from things that went wrong:

- The pool is sized by how many threads actually started, not by how many were asked for.
  If `pthread_create` fails partway (`RLIMIT_NPROC` on a shared cluster is a real case),
  the barrier waits for a count that will never be reached, and it blocks in
  `pthread_cond_wait`, where `SIGINT` does not reach it. The only way out is `SIGKILL`.
- Header parsing uses `strtok_r`, not `strtok`. Eight threads parse headers concurrently
  and `strtok` keeps its cursor in one process-wide static, so two threads walk each
  other's buffers. The header appears once per file, so a domain that loses that race
  stays blank for the entire run, and nothing says so.

### The periodic display

The display is a periodic task on an absolute deadline: the next wake-up time is computed
once and slept to with `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`, rather than
sleeping for a duration after the work. A relative sleep adds the work time to every
period and drifts without bound; an absolute deadline absorbs a slow tick and puts the next
one back on schedule.

The status line reports the scheduler on itself:

| field | meaning |
|---|---|
| `tick now/mean/max 0.5/1.6/121.6 ms` | how long the work in a tick took: read all 116 files, recompute, redraw |
| `jitter 9.7 ms` | worst lateness seen so far: how far past its deadline a period actually started |
| `0/116 late` | deadline misses / ticks completed. A miss is a tick whose work ran past the start of the next period |

In that frame the worst tick did 121.6 ms of work against a 250 ms period, so nothing was
late. There is no hard real-time guarantee here; see [Not verified](#not-verified).

### The window, and why it is a separate process

The Qt window does not read a single `.his` file and does not decide anything. It runs
`his_monitor --stream` as a child process and draws what comes back, one JSON object per
line:

```
{"type":"init", ...}   once: vessel names, regions, final time
{"type":"tick", ...}   every display period: verdict, pressures, arrival times, wave
                       front, the waveform samples since the last tick, systolic per beat
{"type":"exit", ...}   last, with the exit code
```

The split is deliberate. The monitor is the part with the threads, the file tailing and the
verdict, all of it tested and run under the sanitizers. A second implementation of any of it
would sooner or later disagree with the terminal about whether a run is healthy. Here it
cannot: the verdict, dSBP, the arrival times and the wave-front flag in a tick come from the
same functions in `wave.c` that the terminal calls.

It is text on stdout rather than a socket, on purpose. The monitor runs where the solver
runs, usually a server reached over ssh, and with `--ssh` the window starts it there as
`ssh host his_monitor ... --stream`. The lines come back through the ssh channel,
encrypted and authenticated, through any firewall that lets ssh in. A socket server would
need a port, a firewall rule and its own authentication, to do less.

- CI checks both ends of the stream. The window's own parser (`his_gui --check-stream`) is
  run over real monitor output, then the window is run headless against the monitor and
  saves a frame. A change on one side that the other does not follow fails the build
  instead of drawing nonsense.
- It is built to be screenshotted onto a slide. Blue for emphasis, grey for structure,
  black for text, and one red that means "this run is failing" and nothing else. Pressure is
  a single blue ramp, not the terminal's rainbow. Everything is drawn with `QPainter`; there
  is no chart library.
- "Stop the solver" finds the solver by its input file on the command line, shows what it
  found, and asks before sending `SIGTERM`. The first version looked for `sim_1.in` and
  could not find a single real solver: a running solver rewrites its own argument in place,
  and `ps` shows `sim_1 in`. That came out of running the button's pattern against a live
  run, and the pattern now takes both forms.

---

## Problems I ran into

The hard part of this project was not the physiology. It was that a file being written by
another process, right now, breaks almost every assumption a normal parser makes. Items 1
to 5 came up while building the monitor. Items 6 and 7 turned up while building the window:
a second display of the same numbers shows where the first one had been quietly wrong.

### 1. Tailing a file somebody else is writing

The last line is routinely half written. Reading it yields a truncated row that parses as
perfectly valid numbers, which silently corrupts the series. The file position is saved
before each read and restored when the line does not end in a newline. `clearerr()` is also
needed, because once `fgets` has seen EOF the stream latches it and every later read returns
nothing even after the file has grown. But `ferror` has to be checked first, or a genuine
read error looks exactly like "nothing new yet" and the domain polls a dead stream in
silence.

### 2. The column header carries commas of its own

Column names come with argument lists, `P(x,t)`, and the fields are comma-separated.
Splitting on commas cuts that into `P(x` and `t)`, turning seven columns into twelve and
shifting every later column index. The parenthesised parts are stripped before the split.
The column set also varies with the options a run was launched with, so it is read from the
file rather than assumed.

### 3. The default mode has to read the header before skipping to the end

The layout is declared once, at the top of the file, and every data row is discarded until
it has been parsed. Following a file by seeking straight to its end therefore reads the
whole run and accepts nothing. It still counts bytes, so it looks busy, and then it reports
"no samples, wrong base name", blaming a correct command. That was the default path, and it
survived a long time because every screenshot, every benchmark and every test used
`--from-start`, which passes the header on the way through. A test suite can be
systematically blind to the path nobody wrote a test for.

### 4. The pulse foot detector leaves its state on a timer, not a level

Holding a running minimum and firing when the pressure climbs above it is the easy half.
Coming back out is the trap. The obvious condition, waiting for the pressure to return to
the diastolic minimum just recorded, never releases on the first heartbeat: that minimum is
the t=0 startup value, and the solution never goes back to it.

### 5. Two pulse feet have to come from the same heartbeat

The wave reaches the carotid about 15 ms after the aortic root and the femoral about 134 ms
after, so for roughly a sixth of every 730 ms cycle one of them holds this beat's foot and
the other still holds the previous one. Subtracting those gives a negative transit time.
The same trap appeared twice: in the wave speed, where it collapsed to 0 on 30 of 161 live
frames and made the verdict skip that check entirely; and in the arrival time printed in
each cell, which read −656 ms at the ankle. Both now pair the feet by looking for a
difference that falls in a plausible transit window. Pairing by cycle number is the obvious
fix and it is wrong: the counters drift apart over a run, and that version returned 0 on
180 frames out of 180.

### 6. A finished run is quiet because it is finished

The stall watchdog did not know about the final time, so every successful run turned
`STALLED` twenty seconds after its last step, and under `--abort-on-fail` it exited 2,
reporting a finished run as a failed one. Only fixing the verdict would have been worse:
the script would then wait forever for a monitor that never exits. Now a domain that has
reached the final time is excused, and only that domain, so one that stopped halfway is
still caught after all the others have finished. The run is done when every domain is, and
`--abort-on-fail` then exits with the run's own verdict. It showed up as the window
painting a finished run red.

### 7. The beat-to-beat change has to wait for the peak

A new foot resets the running systolic to the pressure at the foot, near diastolic, and it
only climbs back to the real peak a tenth of a second later. Read during that upstroke,
dSBP swung to −18.4 mmHg for 14% of the last beat of the reference run (a beat whose
settled change is 0.49), and the verdict flickered back to `CONVERGING` on every heartbeat
of a run that had long converged. Now the previous settled change is held until the current
beat's peak is final. It showed up as the window's convergence chart disagreeing with its
own verdict badge. The same number had been computed in three places (terminal, stream and
verdict) and is now one function.

Items 1, 3, 5 and 7 do not reproduce against a finished file at all, only against a solver
that is still running.

---

## Reading the display

The terminal view, on a live run. The window shows the same numbers, computed by the same
code; this section reads them off the terminal because every one of them is on screen at
once.

![the terminal view on a live run](docs/screenshot.png)

### The header lines

| line | what it tells you |
|---|---|
| `sim 5.128 / 6.500 s [####....] wall 29s eta 83s` | simulated time and progress. `wall` is how long the monitor has been up, not the solver. `eta` is derived from how fast simulated time advances while the monitor watches, which is what makes it right when you attach partway through. Here, at the observed rate, the solver had been running about 280 s when the monitor attached |
| `CONVERGING cycle 7, cycle-to-cycle SBP change +0.86 mmHg` | the verdict, and the number behind it |
| `aortic root 100.6 / 75.3 mmHg  PP 25.4  dSBP +0.86  cf-PWV 7.54 m/s` | the physiology: systolic/diastolic pressure, pulse pressure, dSBP (the change in systolic pressure between the last two beats whose peaks are final) and pulse wave velocity |
| `116 domains  8 reader threads  116.4 MB read  peak \|U\| 1.04 m/s  tick … jitter … late` | the tool reporting on itself; see [The periodic display](#the-periodic-display) |

### Each cell

```
048 L.Post.Tibial    68  166
│   │                │   │
│   │                │   └─ ms after the wave passed the aortic root
│   │                └───── pressure right now, in mmHg
│   └────────────────────── vessel name, from --names
└────────────────────────── segment number

011 R.Ulnar II      125 > 78
                        │
                        └── this vessel is rising at 45% or more of the
                            fastest-rising vessel in the tree right now,
                            i.e. the wave front is passing through it
```

Arrival times in the screenshot run 0 → 54-85 ms (arms) → 134 ms (femoral) → 163-168 ms
(ankle), which is the pulse propagating down the tree in real time.

Colour is the rainbow map, chosen to match the figures this field already publishes so a
frame can be eyeballed against a reference. It is a poor perceptual choice in general (not
uniform, and bad for red-green colour blindness), and it is used here for cross-checking
against existing figures, not because it is the better colormap.

### Verdicts

Checked in this order; the first match wins.

| # | verdict | condition |
|---|---|---|
| 1 | `STALLED` | nothing arrived anywhere, or nothing new anywhere for `--stall` seconds, unless every domain has reached the final time (a finished run, not a stalled one) |
| 2 | `OFF TARGET` | some domain has written a nan or inf: the solution diverged. Latched, so one bad row is enough for the rest of the run |
| 3 | `STALLED` | some domains are quiet while others stream: never wrote after `--stall` s, or stopped more than 3×`--stall` s ago |
| 4 | `FILLING` | fewer than two heartbeats seen; the pressure field is still growing |
| 5 | `OFF TARGET` | peak \|U\| over `--umax`, naming the domain |
| 6 | `OFF TARGET` | wave speed outside `--tol` of `--pwv-target` |
| 7 | `CONVERGED` | dSBP below 0.5 mmHg: the solution is periodic |
| 8 | `CONVERGING` | 0.5 mmHg or above, or only one beat has settled so far |

Row 6 is deliberately hard to reach: it applies only once the solution has settled, which
here means the cycle-to-cycle change is under 3 mmHg, or five heartbeats have gone by and it
is not going to settle. Judging wave speed during the filling transient kills perfectly good
runs. It did exactly that, at cycle 3, before the guard existed.

Under `--abort-on-fail` the exit code is 2 for `OFF TARGET` or `STALLED`, so a launch
script can act on it. Once every domain has reached the final time, the monitor also exits
by itself with the run's own verdict: 0 if it settled, 2 if it finished off target.

<details>
<summary><b>his_monitor options</b></summary>

```
--names <file>       segment-number to vessel-name table, to label the cells
--from-start         read the .his files from the beginning instead of
                     following only what is appended from now on
--proximal           watch the proximal history point, not the distal one
--period <ms>        display period, default 250, floor 20
--threads <n>        reader threads, default min(8, cores), clamped to 1..64
--pwv-target <m/s>   expected carotid-femoral pulse wave velocity
--tol <pct>          how far the wave speed may drift before it is a failure (10)
--umax <m/s>         peak |U| above which the run is called off target
                     (default 2.0; 0 disables the check)
--stall <s>          seconds without new samples before calling it stalled (20)
--abort-on-fail      exit 2 once the verdict has been OFF TARGET or STALLED for
                     three display periods in a row. Only ever aborts a run that
                     has been seen to advance, so a solver still doing mesh setup
                     shows STALLED but is not killed. Also exits by itself once
                     every domain has reached the final time: 0 if the run
                     settled, 2 if it finished off target
--bench              time a full read: warm-up, 1 thread, N threads, 1 thread
                     again. Prints the four times and exits
--stream             instead of drawing, write one JSON object per display
                     period to stdout, for the window. Works through ssh as it
                     stands: ssh host his_monitor ... --stream
```

`COLUMNS` and `LINES` are honoured when there is no terminal to ask, which is what makes
the layout testable without a tty, and what lets CI run the real binary.

</details>

<details>
<summary><b>his_gui options</b></summary>

```
usage: his_gui <base> [his_monitor options] [options]

--monitor <path>      his_monitor to run. Default: next to his_gui, then
                      ../his_monitor, ../../his_monitor, then PATH
--ssh <host>          run the monitor on <host> over ssh (key auth); the
                      window stays here
--dir <path>          working directory on that host, with --ssh
--capture <dir>       save the window as a PNG every --every seconds
--every <s>           capture interval, default 10
--scale <n>           capture pixel scale, default 2 (crisp on slides)
--quit-after <s>      close after this many seconds
--check-stream <file> parse a saved --stream capture with the window's own
                      parser, report, and exit 0 if it is sound. No display

Everything else goes to his_monitor unchanged: --names, --from-start,
--period, --threads, --pwv-target, --tol, --umax, --stall, --abort-on-fail
```

</details>

<details>
<summary><b>preflight options</b></summary>

```
usage: preflight <file.in> [more.in ...] [--ref good.in] [-v]

--ref <file.in>   compare against an input known to run. Use one from the same
                  family: comparing a cohort against an unrelated model flags
                  all of it. Skipped if it also appears in the list
--zlim <x>        flag an outlet whose Windkessel R exceeds x times the vessel
                  impedance. Off by default; see below
--cfl <x>         flag a domain whose Courant number exceeds x (default 0.40)
--rtol <pct>      with --ref, how far a radius may differ before the input is
                  flagged (default 15). The test is on the log of the ratio, so
                  it is symmetric in ratio and not in percent: 15 flags a radius
                  13.0% smaller, or 15% larger
-v                also print the inputs that pass

exit 0  all clear
exit 1  at least one input flagged, or nothing to check
```

</details>

---

## preflight

Catching a divergence three minutes in is good. Not starting the run at all is better.

```sh
./preflight sim_*.in --ref <an-input-that-runs>.in
```

![preflight screening a radius sweep](docs/preflight.png)

For each domain, `preflight` uses the geometry formulas the input file itself carries to
check that the vessel radius stays positive along the whole segment; that the initial
cross-section is still positive once external pressure has deflated it; that
`CFL = dt·c/dx` is small enough for explicit time stepping, with the wave speed `c` taken
from the same empirical wall law the model uses; and, with `--ref`, how far the radii have
drifted from an input known to run.

### Setting the threshold by experiment

Starting from an input that runs to completion, every radius was scaled by a constant with
the terminal boundary conditions left untouched, and each result was actually run:

| scale | ×0.90 | ×0.80 | ×0.78 | ×0.76 | ×0.75 | ×0.74 | ×0.72 | ×0.70 | ×0.60 | ×0.50 |
|---|---|---|---|---|---|---|---|---|---|---|
| solver | runs | runs | runs | runs | **runs** | **dies** | dies | dies | dies | dies |

The boundary is at about a 25% radius reduction, and it is sharp. The default `--rtol` of
15% flags at a smaller deviation than that on purpose. Over the ten-point sweep above, 5 of
5 failures were caught, and 4 of the 5 working inputs were flagged anyway. The screenshot
shows a six-file subset of the same sweep. There, ×0.80, ×0.76 and ×0.75 are flagged
although all three run, and ×0.74 and ×0.70 are the two that really die.

That asymmetry is deliberate. A false positive costs one look at a file. A false negative
costs a share of a multi-day run.

### What it does not do

`preflight` does not predict divergence from first principles. Run without `--ref`, it
reports `OK` for an input that dies in the first time step: positive radii, positive initial
area, CFL 0.174. It passes every first-principles check there is here. It only catches that
input when given a working configuration to compare against, and the reference has to come
from the same family.

The terminal impedance `R/Z0` is reported but not used to fail a run, because it does not
separate good inputs from bad ones here. The highest value measured anywhere belongs to an
input that runs to completion, and the one that reliably dies is mid-range. There is no
threshold that works, so there is no threshold: just the number, and `--zlim` for anyone
with evidence to set one.

---

## Verified

| | how |
|---|---|
| build | `-O2 -std=c11 -Wall -Wextra -pedantic -Werror`, zero warnings, in CI |
| tests | 83 checks on the parsing/analysis/verdict layers, 13 driving the checker end to end, 12 on the `--stream` byte stream end to end, in CI |
| the real binary under sanitizers | CI generates a synthetic 116-file tree (`tests/make_his.sh`, no solver needed) and runs `his_monitor` itself against it (a `--bench` pass, a live follow-mode run and the stream tests) under ThreadSanitizer, and again under ASan + UBSan. 0 races, 0 errors |
| static analysis | cppcheck, clean, in CI |
| the window | built with `-Werror`; its parser run over real monitor output; run headless against the monitor, drawing a frame; all in CI. The screenshot at the top is from a real 6.5 s solver run: 1,680 ticks, 0 late, and still `CONVERGED` rather than `STALLED` 25 s after the solver had finished, past the 20 s stall window |
| parallel read | 147.7 MB across 116 real files: 1 thread 1.01 s → 8 threads 0.16 s, 6.3× (6.3-6.5 across runs) |
| coverage | all 116 domains report a value in the final frame, at 1, 2 and 8 reader threads alike |

The benchmark runs four passes (a discarded warm-up, then 1 thread, N threads, and 1 thread
again) because measurement order alone changes the answer. The first version measured 1
then N and reported 7.25×; the warm-up pass, which puts both timed passes on the same page
cache, brought it down to 6.3×. The trailing single-threaded pass says whether anything
drifted between the two.

The sanitizer row is worth spelling out, because the obvious way to do it is wrong: the
unit tests link the parsing layer directly and never create a thread, so running them under
ThreadSanitizer proves nothing about the pool. CI builds and runs the actual `his_monitor`
binary instead.

## Not verified

Stated here rather than left for someone to find.

- The `--umax` check is unexercised. Every failure reproducible here dies in the first time
  step, so the gradual velocity rise it exists to catch never occurs. It is a reasoned
  design, not a validated one.
- The wave speed has no end-to-end validation against an independent reference. The wall
  law itself was checked against published values to within 3%, but that validates the
  formula, not this tool's foot detection and path definition.
- There is no RTOS here and no hard real-time guarantee. This is ordinary Linux: an
  absolute-deadline `clock_nanosleep` on `CLOCK_MONOTONIC`, with lateness and misses counted
  and displayed. That is scheduling discipline and instrumentation, not determinism.
- The window's "Stop the solver" was checked against a live run for which processes it
  finds: the solver and nothing else, neither the monitor nor the window. Pressing it is not
  automated, because CI has no solver to stop and the confirmation dialog needs a person.
- The benchmark, the radius sweep and the frame counts above were measured against real
  solver runs on the machine where this was developed. They are not reproducible from this
  repository alone, which carries no solver and no solver data.
- The tool was written during the study in 2025 and run on the lab's machines alongside
  real solver runs; it was collected and published here after graduation. How much time it
  saved was never measured.

---

## Scope

This repository contains only my own code: the C tools, the Qt window and the tests. Qt
itself is not bundled; the window links it dynamically under its LGPL licence. The 1-D
solver whose output it reads is Nektar1D, © King's College London, licensed to me for
non-commercial research use under terms that do not permit redistribution. It is not
included here. There is no source, no input and no output from it, and nothing in this
repository reproduces any part of it. Everything here works from the file formats as
observed at run time, and the synthetic files in `tests/` are generated by the shell script
that sits next to them.

The 116-vessel arterial model and the pulse wave database it comes from are published open
access by Charlton et al.; the vessel name table the `--names` option reads is part of that
public release and is likewise not bundled here.

## Licence

MIT. See [LICENSE](LICENSE).
