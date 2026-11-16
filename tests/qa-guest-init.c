/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Freestanding Linux x86_64 PID 1 for opt-in disposable-guest QA only. */
#define _GNU_SOURCE
#include <asm/unistd.h>
#include <fcntl.h>
#include <linux/reboot.h>

static long
call (long number, long a, long b, long c, long d, long e, long f)
{
  register long r10 __asm__ ("r10") = d;
  register long r8 __asm__ ("r8") = e;
  register long r9 __asm__ ("r9") = f;
  long result;
  __asm__ volatile ("syscall"
                    : "=a"(result)
                    : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                    : "rcx", "r11", "memory", "cc");
  return result;
}

static void
text (const char *message)
{
  unsigned long length = 0;
  while (message[length])
    length++;
  call (__NR_write, 1, (long)message, length, 0, 0, 0);
}

static void
number (unsigned long value)
{
  char buffer[32];
  unsigned int length = 0;
  do
    {
      buffer[length++] = '0' + value % 10;
      value /= 10;
    }
  while (value);
  while (length)
    call (__NR_write, 1, (long)&buffer[--length], 1, 0, 0, 0);
  text ("\n");
}

static void __attribute__ ((noreturn))
poweroff (void)
{
  call (__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0, 0, 0);
  for (;;)
    call (__NR_pause, 0, 0, 0, 0, 0, 0);
}

static void
check (long result, const char *operation)
{
  if (result < 0)
    {
      text ("PIRWA_ERROR ");
      text (operation);
      text (" ");
      number (-result);
      poweroff ();
    }
}

void __attribute__ ((noreturn))
guest_main (void)
{
  check (call (__NR_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0), "devtmpfs");
  long mounted = -1;
  const long delay[] = { 0, 250000000 };
  for (unsigned int i = 0; i < 40 && mounted < 0; i++)
    {
      mounted = call (__NR_mount, (long)"/dev/vda", (long)"/mnt", (long)"ext4", 0, 0, 0);
      if (mounted < 0)
        call (__NR_nanosleep, (long)delay, 0, 0, 0, 0, 0);
    }
  check (mounted, "ext4");
  long input = call (__NR_open, (long)"/dev/ttyS0", O_RDWR | O_NONBLOCK, 0, 0, 0, 0);
  check (input, "serial");
  long disk = call (__NR_open, (long)"/mnt/heartbeat", O_RDWR | O_CREAT | O_DSYNC, 0600, 0, 0, 0);
  check (disk, "heartbeat");
  typedef struct
  {
    char magic[8];
    unsigned long counter;
    unsigned long inverse;
  } Record;
  Record record = { { 'P', 'I', 'R', 'W', 'A', '0', '1', '\n' }, 0, ~0UL };
  Record previous;
  long size = call (__NR_read, disk, (long)&previous, sizeof previous, 0, 0, 0);
  check (size, "read");
  if (size)
    {
      long valid = size == (long)sizeof previous && previous.inverse == ~previous.counter;
      for (unsigned int i = 0; i < sizeof record.magic; i++)
        valid = valid && previous.magic[i] == record.magic[i];
      check (valid ? 0 : -1, "record-integrity");
      record = previous;
    }
  text ("PIRWA_BOOT ");
  number (record.counter);
  for (;;)
    {
      char command[16];
      long length = call (__NR_read, input, (long)command, sizeof command, 0, 0, 0);
      for (long i = 0; i < length; i++)
        if (command[i] == 'Q')
          {
            check (call (__NR_fsync, disk, 0, 0, 0, 0, 0), "final-fsync");
            check (call (__NR_close, disk, 0, 0, 0, 0, 0), "close");
            check (call (__NR_umount2, (long)"/mnt", 0, 0, 0, 0, 0), "unmount");
            text ("PIRWA_CLEAN ");
            number (record.counter);
            poweroff ();
          }
      record.counter++;
      record.inverse = ~record.counter;
      check (call (__NR_lseek, disk, 0, 0, 0, 0, 0), "seek");
      long written = call (__NR_write, disk, (long)&record, sizeof record, 0, 0, 0);
      check (written == (long)sizeof record ? 0 : -1, "write");
      check (call (__NR_fdatasync, disk, 0, 0, 0, 0, 0), "fdatasync");
      text ("PIRWA_HEARTBEAT ");
      number (record.counter);
      call (__NR_nanosleep, (long)delay, 0, 0, 0, 0, 0);
    }
}
