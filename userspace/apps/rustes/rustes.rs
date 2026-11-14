#![no_std]
#![no_main]

use rsbr::prelude::*;
use rsbr::time::{
    self,
    getitimer,
    ITIMER_REAL,
    ITIMER_VIRTUAL,
    ITIMER_PROF,
};

const NS_PER_SEC: u128 = 1_000_000_000;

const BENCH_SAMPLES: u32 = 7;
const BENCH_ITERATIONS: u32 = 512;
const MONOTONIC_ITERATIONS: u32 = 4096;
const SLEEP_REPETITIONS: u32 = 3;

fn clock_ns(clock: i32) -> Option<u128> {
    let ts = time::clock_gettime(clock).ok()?;

    if ts.tv_sec < 0
        || ts.tv_nsec < 0
        || ts.tv_nsec >= 1_000_000_000
    {
        return None;
    }

    Some(
        (ts.tv_sec as u128) * NS_PER_SEC
            + ts.tv_nsec as u128
    )
}

fn monotonic_ns() -> Option<u128> {
    clock_ns(time::CLOCK_MONOTONIC)
}

fn print_clock_snapshot(clock: i32, name: &str) {
    match time::clock_gettime(clock) {
        Ok(ts) => {
            println!(
                "  {:<25} {}.{:09}",
                name,
                ts.tv_sec,
                ts.tv_nsec
            );
        }
        Err(error) => {
            println!("  {:<25} FAILED: {}", name, error);
        }
    }
}

fn print_time_snapshot() {
    println!("\n[1] CLOCK SNAPSHOT");

    print_clock_snapshot(
        time::CLOCK_REALTIME,
        "CLOCK_REALTIME"
    );

    print_clock_snapshot(
        time::CLOCK_MONOTONIC,
        "CLOCK_MONOTONIC"
    );

    print_clock_snapshot(
        time::CLOCK_PROCESS_CPUTIME_ID,
        "PROCESS_CPUTIME"
    );

    print_clock_snapshot(
        time::CLOCK_THREAD_CPUTIME_ID,
        "THREAD_CPUTIME"
    );

    match time::realtime() {
        Ok(ts) => {
            println!(
                "  {:<25} {}.{:09}",
                "realtime()",
                ts.tv_sec,
                ts.tv_nsec
            );
        }
        Err(error) => {
            println!("  realtime() FAILED: {}", error);
        }
    }

    match time::gettimeofday() {
        Ok(tv) => {
            println!(
                "  gettimeofday()           {}.{:06}",
                tv.tv_sec,
                tv.tv_usec
            );
        }
        Err(error) => {
            println!("  gettimeofday() FAILED: {}", error);
        }
    }

    match time::time() {
        Ok(value) => {
            println!("  time()                   {}", value);
        }
        Err(error) => {
            println!("  time() FAILED: {}", error);
        }
    }
}

fn print_clock_resolution() {
    println!("\n[2] CLOCK RESOLUTION");

    match time::clock_getres(time::CLOCK_MONOTONIC) {
        Ok(resolution) => {
            println!(
                "  Reported resolution: {} sec, {} ns",
                resolution.tv_sec,
                resolution.tv_nsec
            );
        }
        Err(error) => {
            println!("  clock_getres FAILED: {}", error);
        }
    }

    println!(
        "  Note: clock_getres currently ignores its clock argument."
    );
}

fn check_monotonic() {
    println!("\n[3] MONOTONIC CLOCK VALIDATION");

    let mut previous = match monotonic_ns() {
        Some(value) => value,
        None => {
            println!("  FAILED: cannot read CLOCK_MONOTONIC");
            return;
        }
    };

    let mut regressions = 0u32;
    let mut identical = 0u32;

    let mut min_step = u128::MAX;
    let mut max_step = 0u128;

    for _ in 0..MONOTONIC_ITERATIONS {
        let current = match monotonic_ns() {
            Some(value) => value,
            None => {
                println!("  FAILED: clock_gettime returned invalid data");
                return;
            }
        };

        if current < previous {
            regressions += 1;
        } else if current == previous {
            identical += 1;
        } else {
            let step = current - previous;

            if step < min_step {
                min_step = step;
            }

            if step > max_step {
                max_step = step;
            }
        }

        previous = current;
    }

    println!(
        "  Samples:                  {}",
        MONOTONIC_ITERATIONS
    );

    println!(
        "  Clock regressions:        {}",
        regressions
    );

    println!(
        "  Identical readings:       {}",
        identical
    );

    if min_step != u128::MAX {
        println!(
            "  Minimum observed step:    {} ns",
            min_step
        );

        println!(
            "  Maximum observed step:    {} ns",
            max_step
        );
    } else {
        println!("  No positive clock step observed.");
    }

    if regressions == 0 {
        println!("  Result: PASS");
    } else {
        println!("  Result: FAIL");
    }
}

fn benchmark_calls<F>(name: &str, mut operation: F)
where
    F: FnMut() -> Option<u64>,
{
    let mut total = 0u128;
    let mut min = u128::MAX;
    let mut max = 0u128;
    let mut checksum = 0u64;

    for _ in 0..BENCH_SAMPLES {
        let start = match monotonic_ns() {
            Some(value) => value,
            None => {
                println!("  {:<30} FAILED: monotonic clock", name);
                return;
            }
        };

        for _ in 0..BENCH_ITERATIONS {
            match operation() {
                Some(value) => {
                    checksum = checksum.wrapping_add(value);
                }
                None => {
                    println!("  {:<30} FAILED: operation", name);
                    return;
                }
            }
        }

        let end = match monotonic_ns() {
            Some(value) => value,
            None => {
                println!("  {:<30} FAILED: monotonic clock", name);
                return;
            }
        };

        if end < start {
            println!("  {:<30} FAILED: clock regression", name);
            return;
        }

        let elapsed = end - start;
        let per_call = elapsed / BENCH_ITERATIONS as u128;

        total += per_call;

        if per_call < min {
            min = per_call;
        }

        if per_call > max {
            max = per_call;
        }
    }

    let average = total / BENCH_SAMPLES as u128;

    println!(
        "  {:<30} avg {:>8} ns | min {:>8} | max {:>8}",
        name,
        average,
        min,
        max
    );

    // The checksum makes the operation's return values observable.
    let _ = checksum;
}

fn benchmark_syscalls() {
    println!("\n[4] SYSCALL OVERHEAD");

    println!(
        "  {} samples x {} calls per sample",
        BENCH_SAMPLES,
        BENCH_ITERATIONS
    );

    benchmark_calls(
        "clock_gettime(MONOTONIC)",
        || {
            time::clock_gettime(time::CLOCK_MONOTONIC)
                .ok()
                .map(|ts| {
                    (ts.tv_sec as u64)
                        .wrapping_add(ts.tv_nsec as u64)
                })
        },
    );

    benchmark_calls(
        "clock_gettime(REALTIME)",
        || {
            time::clock_gettime(time::CLOCK_REALTIME)
                .ok()
                .map(|ts| {
                    (ts.tv_sec as u64)
                        .wrapping_add(ts.tv_nsec as u64)
                })
        },
    );

    benchmark_calls(
        "clock_gettime(PROCESS)",
        || {
            time::clock_gettime(time::CLOCK_PROCESS_CPUTIME_ID)
                .ok()
                .map(|ts| {
                    (ts.tv_sec as u64)
                        .wrapping_add(ts.tv_nsec as u64)
                })
        },
    );

    benchmark_calls(
        "clock_gettime(THREAD)",
        || {
            time::clock_gettime(time::CLOCK_THREAD_CPUTIME_ID)
                .ok()
                .map(|ts| {
                    (ts.tv_sec as u64)
                        .wrapping_add(ts.tv_nsec as u64)
                })
        },
    );

    benchmark_calls(
        "realtime()",
        || {
            time::realtime()
                .ok()
                .map(|ts| {
                    (ts.tv_sec as u64)
                        .wrapping_add(ts.tv_nsec as u64)
                })
        },
    );

    benchmark_calls(
        "gettimeofday()",
        || {
            time::gettimeofday()
                .ok()
                .map(|tv| {
                    (tv.tv_sec as u64)
                        .wrapping_add(tv.tv_usec as u64)
                })
        },
    );

    benchmark_calls(
        "time()",
        || {
            time::time()
                .ok()
                .map(|value| value as u64)
        },
    );

    benchmark_calls(
        "clock_getres()",
        || {
            time::clock_getres(time::CLOCK_MONOTONIC)
                .ok()
                .map(|ts| {
                    (ts.tv_sec as u64)
                        .wrapping_add(ts.tv_nsec as u64)
                })
        },
    );

    benchmark_calls(
        "getitimer(REAL)",
        || {
            getitimer(ITIMER_REAL)
                .ok()
                .map(|state| {
                    state.interval_ticks
                        .wrapping_add(state.deadline_ticks)
                })
        },
    );

    benchmark_calls(
        "getitimer(VIRTUAL)",
        || {
            getitimer(ITIMER_VIRTUAL)
                .ok()
                .map(|state| {
                    state.interval_ticks
                        .wrapping_add(state.deadline_ticks)
                })
        },
    );

    benchmark_calls(
        "getitimer(PROF)",
        || {
            getitimer(ITIMER_PROF)
                .ok()
                .map(|state| {
                    state.interval_ticks
                        .wrapping_add(state.deadline_ticks)
                })
        },
    );
}

fn print_timer_state(which: i32, name: &str) {
    match getitimer(which) {
        Ok(state) => {
            println!(
                "  {:<20} interval={:<12} deadline={}",
                name,
                state.interval_ticks,
                state.deadline_ticks
            );
        }
        Err(error) => {
            println!("  {:<20} FAILED: {}", name, error);
        }
    }
}

fn print_timer_snapshots() {
    println!("\n[5] INTERVAL TIMER SNAPSHOT");

    println!("  Values are expressed in kernel ticks.");

    print_timer_state(ITIMER_REAL, "ITIMER_REAL");
    print_timer_state(ITIMER_VIRTUAL, "ITIMER_VIRTUAL");
    print_timer_state(ITIMER_PROF, "ITIMER_PROF");
}

fn nanosleep_complete(ns: u64) -> bool {
    let mut remaining = time::TimeSpec {
        tv_sec: (ns / 1_000_000_000) as i64,
        tv_nsec: (ns % 1_000_000_000) as i64,
    };

    loop {
        match time::nanosleep(remaining) {
            Ok(None) => return true,
            Ok(Some(rem)) => {
                remaining = rem;
            }
            Err(_) => return false,
        }
    }
}

fn benchmark_sleep<F>(
    name: &str,
    requested_ns: u64,
    mut sleeper: F,
)
where
    F: FnMut() -> bool,
{
    let mut total = 0u128;
    let mut min = u128::MAX;
    let mut max = 0u128;

    for _ in 0..SLEEP_REPETITIONS {
        let start = match monotonic_ns() {
            Some(value) => value,
            None => {
                println!("  {} FAILED: monotonic clock", name);
                return;
            }
        };

        if !sleeper() {
            println!("  {} FAILED: sleep operation", name);
            return;
        }

        let end = match monotonic_ns() {
            Some(value) => value,
            None => {
                println!("  {} FAILED: monotonic clock", name);
                return;
            }
        };

        if end < start {
            println!("  {} FAILED: clock regression", name);
            return;
        }

        let elapsed = end - start;

        total += elapsed;

        if elapsed < min {
            min = elapsed;
        }

        if elapsed > max {
            max = elapsed;
        }
    }

    let average = total / SLEEP_REPETITIONS as u128;
    let difference = average as i128 - requested_ns as i128;

    println!(
        "  {:<25} target {:>8} ns | avg {:>10} ns | min {:>10} | max {:>10} | delta {:+} ns",
        name,
        requested_ns,
        average,
        min,
        max,
        difference
    );
}

fn benchmark_sleep_functions() {
    println!("\n[6] SLEEP ACCURACY");

    println!(
        "  {} repetitions per test",
        SLEEP_REPETITIONS
    );

    println!("\n  sleep_ms:");

    benchmark_sleep(
        "sleep_ms(1)",
        1_000_000,
        || time::sleep_ms(1).is_ok(),
    );

    benchmark_sleep(
        "sleep_ms(5)",
        5_000_000,
        || time::sleep_ms(5).is_ok(),
    );

    benchmark_sleep(
        "sleep_ms(10)",
        10_000_000,
        || time::sleep_ms(10).is_ok(),
    );

    println!("\n  usleep:");

    benchmark_sleep(
        "usleep(1000)",
        1_000_000,
        || time::usleep(1_000).is_ok(),
    );

    benchmark_sleep(
        "usleep(5000)",
        5_000_000,
        || time::usleep(5_000).is_ok(),
    );

    benchmark_sleep(
        "usleep(10000)",
        10_000_000,
        || time::usleep(10_000).is_ok(),
    );

    println!("\n  nanosleep:");

    benchmark_sleep(
        "nanosleep(1ms)",
        1_000_000,
        || nanosleep_complete(1_000_000),
    );

    benchmark_sleep(
        "nanosleep(5ms)",
        5_000_000,
        || nanosleep_complete(5_000_000),
    );

    benchmark_sleep(
        "nanosleep(10ms)",
        10_000_000,
        || nanosleep_complete(10_000_000),
    );
}

fn run_benchmark() {
    print_time_snapshot();
    print_clock_resolution();
    check_monotonic();
    print_timer_snapshots();
    benchmark_syscalls();
    benchmark_sleep_functions();
}

#[no_mangle]
pub extern "Rust" fn main() -> i32 {
    println!();
    println!("========================================");
    println!("          RSBR TIME BENCHMARK");
    println!("========================================");

    match getpid() {
        Ok(pid) => println!("Process ID: {}", pid),
        Err(error) => {
            eprintln!("getpid failed: {}", error);
        }
    }

    println!("Mode: no_std userspace");
    println!("Allocation: disabled");
    println!("Timer: CLOCK_MONOTONIC");

    run_benchmark();

    println!();
    println!("========================================");
    println!("          BENCHMARK COMPLETE");
    println!("========================================");

    0
}
