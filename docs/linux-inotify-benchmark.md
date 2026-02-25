# Linux inotify benchmark vs master

## Context

- Dataset: `/home/julian/repos/knime_workflows` (~71,589 directories)
- Platform: Linux
- Compared branches:
  - `master` (`b3308a9`)
  - `improve-linux` (flat-index refactor + second RAM optimization pass)

## Methodology

- 3 runs per branch
- Same benchmark harness and options on both branches
- Command pattern:
  - `node --expose-gc /tmp/nsfw_bench_inline.js /home/julian/repos/knime_workflows 500 1000 360000 60000`
  - `/usr/bin/time -f "elapsed_s=%e max_rss_kb=%M" ...`

## Raw samples

### improve-linux

- Run 1: `start_ms=670.24`, `total_ms=1735.95`, `max_rss_kb=85492`
- Run 2: `start_ms=688.44`, `total_ms=1749.89`, `max_rss_kb=85160`
- Run 3: `start_ms=689.31`, `total_ms=1753.44`, `max_rss_kb=85148`

### master

- Run 1: `start_ms=778.34`, `total_ms=1833.99`, `max_rss_kb=96128`
- Run 2: `start_ms=775.37`, `total_ms=1829.50`, `max_rss_kb=96196`
- Run 3: `start_ms=778.91`, `total_ms=1831.95`, `max_rss_kb=93828`

## Averages and deltas (improve-linux vs master)

- `start_ms`: **682.66** vs **777.54** → **-12.20%**
- `total_ms`: **1746.43** vs **1831.81** → **-4.66%**
- `max_rss_kb`: **85266.67** vs **95384.00** → **-10.61%**

## Conclusion

The Linux refactor reduces peak memory usage by ~10.6% and also improves startup and total runtime on a large recursive directory tree benchmark.
