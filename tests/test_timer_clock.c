/* LLM-assisted regression test for PIT clock source.
 *
 * The i8253 PIT must run from monotonic hardware time.  BIOS wall-clock
 * time deliberately remains based on gettimeofday().  This test supplies
 * independent fake monotonic and wall clocks to timer.c and verifies that
 * the two domains cannot affect each other.
 */

#include "dbg.h"
#include "emu.h"
#include "timer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

uint8_t memory[1 << 20];
static struct timespec fake_mono;
static struct timeval fake_wall;

int
clock_gettime (clockid_t clock_id, struct timespec *ts)
{
  if (clock_id != CLOCK_MONOTONIC)
    {
      return -1;
    }

  *ts = fake_mono;
  return 0;
}

int
gettimeofday (struct timeval *tv, void *tz)
{
  (void)tz;
  *tv = fake_wall;
  return 0;
}

void
debug (enum debug_type type, const char *format, ...)
{
  (void)type;
  (void)format;
}

unsigned
cpuGetAX (void)
{
  return 0;
}
unsigned
cpuGetBX (void)
{
  return 0;
}
unsigned
cpuGetCX (void)
{
  return 0;
}
unsigned
cpuGetDX (void)
{
  return 0;
}
void
cpuSetAX (unsigned v)
{
  (void)v;
}
void
cpuSetCX (unsigned v)
{
  (void)v;
}
void
cpuSetDX (unsigned v)
{
  (void)v;
}

static void
fail (const char *what, unsigned long got, unsigned long expected)
{
  fprintf (stderr, "test_timer_clock: %s: got %lu, expected %lu\n", what, got,
           expected);
  exit (1);
}

static uint16_t
latch_timer0 (void)
{
  port_timer_write (0x43, 0x00);
  unsigned lo = port_timer_read (0x40);
  unsigned hi = port_timer_read (0x40);
  return (uint16_t)(lo | (hi << 8));
}

int
main (void)
{
  fake_mono.tv_sec = 0;
  fake_mono.tv_nsec = 0;
  fake_wall.tv_sec = 1700000000;
  fake_wall.tv_usec = 0;

  /* Channel 0, LSB/MSB, mode 2, divisor 65536. */
  port_timer_write (0x43, 0x34);
  port_timer_write (0x40, 0x00);
  port_timer_write (0x40, 0x00);

  uint16_t pit = latch_timer0 ();
  if (pit != 0x0000)
    {
      fail ("initial PIT count", pit, 0x0000);
    }

  /* 500 ns is about 0.5966 PIT counts and therefore rounds to one count.
   * Simultaneously move civil time backwards by an hour.  A PIT driven by
   * gettimeofday() would jump; a monotonic, nanosecond clock must not. */
  fake_mono.tv_nsec = 500;
  fake_wall.tv_sec -= 3600;
  pit = latch_timer0 ();
  if (pit != 0xFFFF)
    {
      fail ("PIT after wall-clock jump", pit, 0xFFFF);
    }

  /* At 100 us, the existing 105/88 conversion gives 119 PIT counts. */
  fake_mono.tv_nsec = 100000;
  fake_wall.tv_sec += 7200;
  pit = latch_timer0 ();
  if (pit != 0xFF89)
    {
      fail ("PIT high-resolution progression", pit, 0xFF89);
    }

  /* BIOS time remains tied to civil time, not CLOCK_MONOTONIC. */
  fake_wall.tv_sec = 1700000000;
  fake_wall.tv_usec = 0;
  update_timer ();
  uint32_t bios0 = get_bios_timer ();

  fake_mono.tv_sec += 3600;
  update_timer ();
  if (get_bios_timer () != bios0)
    {
      fail ("BIOS timer changed with monotonic clock", get_bios_timer (),
            bios0);
    }

  fake_wall.tv_sec += 10;
  update_timer ();
  uint32_t bios1 = get_bios_timer ();
  unsigned delta = (bios1 + 0x1800B0u - bios0) % 0x1800B0u;
  if (delta < 181 || delta > 183)
    {
      fail ("BIOS timer did not follow wall clock", delta, 182);
    }

  puts ("Timer clock tests: ALL PASS");
  return 0;
}
