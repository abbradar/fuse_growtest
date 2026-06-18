#define _GNU_SOURCE

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STAT_THREADS 4
#define MAX_HITS 20
#define MAP_BYTES ((size_t)1 << 30)

static const char *path;
static volatile int stop;

static void *stat_hammer(void *arg) {
  (void)arg;
  struct stat st;
  while (!stop)
    stat(path, &st);
  return NULL;
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s MOUNTPOINT/testfile\n", argv[0]);
    return 2;
  }
  path = argv[1];

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    return 2;
  }

  const unsigned char *base =
      mmap(NULL, MAP_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
  if (base == MAP_FAILED) {
    perror("mmap");
    return 2;
  }

  pthread_t threads[STAT_THREADS];
  for (int i = 0; i < STAT_THREADS; i++)
    pthread_create(&threads[i], NULL, stat_hammer, NULL);

  off_t offset = 0;
  int hits = 0;
  double t0 = now_sec();

  printf("mmap-reading %s, checking for stale zeros ...\n", path);

  while (hits < MAX_HITS) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
      perror("fstat");
      break;
    }
    off_t size = st.st_size;
    if (offset >= size) {
      nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL); /* 1 ms */
      continue;
    }
    if ((size_t)size > MAP_BYTES) {
      fprintf(stderr, "file outgrew the %zu-byte mapping window; stopping\n",
              (size_t)MAP_BYTES);
      break;
    }

    const unsigned char *zero = memchr(base + offset, 0x00, size - offset);
    if (zero) {
      off_t foff = zero - base;
      printf("  STALE ZERO at offset %lld (page %lld, +%lld) t=%.3fs\n",
             (long long)foff, (long long)(foff / 4096),
             (long long)(foff % 4096), now_sec() - t0);
      hits++;
    }
    offset = size;
  }

  stop = 1;
  for (int i = 0; i < STAT_THREADS; i++)
    pthread_join(threads[i], NULL);

  if (hits) {
    printf("\nBug reproduced: %d stale-zero region(s) in %.1fs (%lld bytes "
           "read).\n",
           hits, now_sec() - t0, (long long)offset);
    return 1;
  }
  printf("No corruption after %lld bytes.\n", (long long)offset);
  return 0;
}
