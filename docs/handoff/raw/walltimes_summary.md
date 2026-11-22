| dataset | mode | binary | wall ms min / median / max | CPU ms median | peak RSS MiB | majflt median | cgroup throttled (nr / ms, median) |
|---|---|---|---|---|---|---|---|
| 20M | native | main_eeb320a | 596 / 605 / 648 | 4399 | 3566 | 0 | - |
| 20M | native | main_e5bea99 | 903 / 926 / 1002 | 6943 | 3847 | 0 | - |
| 20M | docker | main_eeb320a | 734 / 749 / 1117 | 4595 | 1918 | - | 1 / 0 |
| 20M | docker | main_e5bea99 | 946 / 1353 / 1407 | 6409 | 2311 | - | 1 / 2 |
| 60M | native | main_eeb320a | 2419 / 3138 / 3518 | 15888 | 11015 | 25581 | - |
| 60M | native | main_e5bea99 | 4542 / 4776 / 5176 | 30006 | 10672 | 18645 | - |
| 60M | docker | main_eeb320a | 2416 / 2461 / 2495 | 16222 | 5922 | - | 3 / 4 |
| 60M | docker | main_e5bea99 | 4198 / 4207 / 4606 | 29378 | 6137 | - | 3 / 1 |
