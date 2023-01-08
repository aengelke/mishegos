
#include "mish_common.h"
#include "mutator.h"

#include <assert.h>
#include <dlfcn.h>
#include <err.h>
#include <pthread.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/futex.h>

#define WITH_FUTEX

typedef struct {
  _Atomic uint32_t val;
#ifdef WITH_FUTEX
  _Atomic uint32_t waiters;
#endif
} mish_atomic_uint;

static void mish_atomic_wait_for(mish_atomic_uint *var, uint32_t target) {
  uint32_t old;
  size_t cnt = 0;
  while ((old = atomic_load(&var->val)) != target) {
#ifdef __x86_64__
    __asm__ volatile("pause");
#endif
    (void)cnt;
#ifdef WITH_FUTEX
    if (++cnt > 10000) {
      atomic_fetch_add_explicit(&var->waiters, 1, memory_order_relaxed);
      syscall(SYS_futex, &var->val, FUTEX_WAIT, old, NULL);
      atomic_fetch_sub_explicit(&var->waiters, 1, memory_order_relaxed);
    }
#endif
  }
}

static uint32_t mish_atomic_fetch_add(mish_atomic_uint *var, uint32_t val) {
  return atomic_fetch_add(&var->val, val);
}

static uint32_t mish_atomic_load(mish_atomic_uint *var) {
  return atomic_load(&var->val);
}

static void mish_atomic_store(mish_atomic_uint *var, uint32_t val) {
  atomic_store(&var->val, val);
}

static void mish_atomic_notify(mish_atomic_uint *var) {
#ifdef WITH_FUTEX
  if (atomic_load_explicit(&var->waiters, memory_order_relaxed))
    syscall(SYS_futex, &var->val, FUTEX_WAKE, INT_MAX);
#endif
}

#define MISHEGOS_NUM_SLOTS_PER_CHUNK 4096
#define MISHEGOS_NUM_CHUNKS 16

typedef struct {
  mish_atomic_uint generation;
  mish_atomic_uint remaining_workers;
  uint32_t input_count;
  input_slot inputs[MISHEGOS_NUM_SLOTS_PER_CHUNK];
} input_chunk;

typedef struct {
  mish_atomic_uint remaining;
  output_slot outputs[MISHEGOS_NUM_SLOTS_PER_CHUNK];
} output_chunk;

struct worker_config {
  size_t soname_len;
  const char *soname;
  int workerno;
  input_chunk *input_chunks;
  output_chunk *output_chunks;
  size_t start_gen;
  size_t start_idx;
  bool sigchld;
  pthread_t thread;
  pid_t pid;
};

static struct worker_config workers[MISHEGOS_MAX_NWORKERS];

static void *alloc_shared(size_t size) {
  void *res = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON | MAP_POPULATE, -1, 0);
  if (res == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  return res;
}

static void *worker(void *wc_vp) {
  const struct worker_config *wc = wc_vp;
  void *so = dlopen(wc->soname, RTLD_LAZY);
  if (!so) {
    perror(wc->soname);
    return NULL;
  }

  void (*worker_ctor)() = (void (*)())dlsym(so, "worker_ctor");
  void (*worker_dtor)() = (void (*)())dlsym(so, "worker_dtor");
  typedef void (*try_decode_t)(output_slot * result, uint8_t * raw_insn, uint8_t length);
  try_decode_t try_decode = (try_decode_t)dlsym(so, "try_decode");
  char *worker_name = *((char **)dlsym(so, "worker_name"));

  if (worker_ctor != NULL) {
    worker_ctor();
  }

  uint32_t gen = wc->start_gen;
  size_t idx = wc->start_idx;

  input_chunk *input_chunks = wc->input_chunks;
  output_chunk *output_chunks = wc->output_chunks;
  while (1) {
    mish_atomic_wait_for(&input_chunks[idx].generation, gen);

    /* Track remaining slots; if we crash, we know where we are. If we start
     * with a non-zero remaining count, we continue where we left, but skip the
     * slot that caused us to crash. */
    size_t old_remaining = mish_atomic_load(&output_chunks[idx].remaining);
    size_t start = old_remaining == 0 ? 0 : input_chunks[idx].input_count - old_remaining + 1;
    mish_atomic_store(&output_chunks[idx].remaining, input_chunks[idx].input_count - start);
    for (size_t i = start; i < input_chunks[idx].input_count; i++) {
      output_chunks[idx].outputs[i].len = 0;
      output_chunks[idx].outputs[i].ndecoded = 0;
      try_decode(&output_chunks[idx].outputs[i], input_chunks[idx].inputs[i].raw_insn,
                 input_chunks[idx].inputs[i].len);
      /* Note: this is no atomic subtraction. It atomic, however, to ensure that
       * the decode result is written to memory before we decrement the counter */
      mish_atomic_store(&output_chunks[idx].remaining, input_chunks[idx].input_count - i - 1);
    }

    if (mish_atomic_fetch_add(&input_chunks[idx].remaining_workers, -1) == 1)
      mish_atomic_notify(&input_chunks[idx].remaining_workers);

    /* Not getting a full chunk indicates that we are exiting. */
    if (input_chunks[idx].input_count != MISHEGOS_NUM_SLOTS_PER_CHUNK)
      break;

    idx++;
    if (idx == MISHEGOS_NUM_CHUNKS) {
      idx = 0;
      gen++;
    }
  }

  if (worker_dtor != NULL) {
    worker_dtor();
  }
  dlclose(so);

  return NULL;
}

/* By default, filter all inputs which all decoders identify as invalid. */
static int filter_min_success = 1;
static int filter_max_success = MISHEGOS_MAX_NWORKERS;
static bool filter_ndecoded_same = false;

static void process(size_t slot, size_t idx, input_chunk *input_chunks, int nworkers,
                    struct worker_config *workers) {
  int num_success = 0;
  bool ndecoded_same = true;
  int last_ndecoded = -1;
  for (int j = 0; j < nworkers; j++) {
    output_slot *output = &workers[j].output_chunks[slot].outputs[idx];
    num_success += output->status == S_SUCCESS;
    if (output->status == S_SUCCESS) {
      if (last_ndecoded == -1)
        last_ndecoded = output->ndecoded;
      else if (last_ndecoded != output->ndecoded)
        ndecoded_same = false;
    }
  }
  if (num_success >= filter_min_success && num_success <= filter_max_success)
    goto keep;
  if (filter_ndecoded_same && !ndecoded_same)
    goto keep;
  return;

keep:;
  fwrite(&nworkers, sizeof(nworkers), 1, stdout);

  input_slot *input = &input_chunks[slot].inputs[idx];
  fwrite(input, sizeof(*input), 1, stdout);
  for (int j = 0; j < nworkers; j++) {
    fwrite(&workers[j].soname_len, sizeof(workers[j].soname_len), 1, stdout);
    fwrite(workers[j].soname, 1, workers[j].soname_len, stdout);

    output_slot *output = &workers[j].output_chunks[slot].outputs[idx];
    static_assert(offsetof(output_slot, result) == sizeof(output_slot) - MISHEGOS_DEC_MAXLEN,
                  "expect result buffer to be at end of slot");
    fwrite(output, sizeof(*output) - MISHEGOS_DEC_MAXLEN + output->len, 1, stdout);
  }
}

static int worker_for_pid(pid_t pid) {
  for (int i = 0; i < MISHEGOS_MAX_NWORKERS; i++) {
    if (workers[i].pid == pid) {
      return i;
    }
  }
  return -1;
}

static bool thread_mode = false;

static void worker_start(struct worker_config *wc) {
  if (thread_mode) {
    pthread_create(&wc->thread, NULL, worker, wc);
  } else {
    /* pipe to notify child that we are ready. */
    int pipe_fds[2];
    char tmp = 0;
    if (pipe(pipe_fds) < 0) {
      perror("pipe");
      exit(1);
    }

    pid_t child = fork();
    if (child < 0) {
      perror("fork");
      exit(1);
    } else if (child == 0) {
      prctl(PR_SET_PDEATHSIG, SIGHUP);
      close(pipe_fds[1]);
      if (read(pipe_fds[0], &tmp, 1) != 1) {
        /* parent died without us being killed by SIGHUP -- so exit. */
        exit(1);
      }
      close(pipe_fds[0]);
      worker(wc);
      exit(0);
    }
    wc->pid = child;
    close(pipe_fds[0]);
    write(pipe_fds[1], &tmp, 1);
    close(pipe_fds[1]);
  }
}

static void sigchld_handler(int sig) {
  (void)sig;

  /* Multiple children might have died at the same time, but we get only one signal. */
  int wstatus;
  pid_t wpid;
  while ((wpid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
    int workerno = worker_for_pid(wpid);
    assert(workerno >= 0);
    if (workerno < 0) {
      /* worker died before we even had the chance to store its pid. */
      abort();
    }
    input_chunk *ic = workers[workerno].input_chunks;
    output_chunk *oc = workers[workerno].output_chunks;
    for (size_t widx = 0; widx < MISHEGOS_NUM_CHUNKS; widx++) {
      uint32_t remaining = mish_atomic_load(&oc[widx].remaining);
      if (remaining == 0)
        continue;
      /* we found the position where the worker crashed. */
      oc[widx].outputs[ic[widx].input_count - remaining].status = S_CRASH;
      /* update generation and chunk index so that worker can restart. */
      workers[workerno].start_gen = mish_atomic_load(&ic[widx].generation);
      workers[workerno].start_idx = widx;
      /* Mark worker as sigchld-received s.t. we can restart them. We obviously
       * can't do that in a signal handler. */
      workers[workerno].sigchld = true;
      /* Reduce remaining_workers temporarily s.t. we always wake up. No need to
       * explicitly wake, however: the futex syscall will be restarted and
       * detect that the value changed */
      mish_atomic_fetch_add(&ic[widx].remaining_workers, -1);
      break;
    }
    /* We might get here because the worker terminated ordinarily -- ignore.
     * There's also the case that the worker crashed outside decoding. This must
     * be a bug and therefore should never happen(TM). Ignore this case, too. */
  }
}

int main(int argc, char **argv) {
  const char *mutator_name = NULL;

  int opt;
  while ((opt = getopt(argc, argv, "htm:s:n")) != -1) {
    switch (opt) {
    case 't':
      thread_mode = false;
      break;
    case 'm':
      mutator_name = optarg;
      break;
    case 's': {
      char *next;
      /* Both values are capped to nworkers below, s.t. -1 => nworkers - 1. */
      filter_min_success = strtol(optarg, &next, 0);
      if (*next == ':')
        filter_max_success = strtol(next + 1, &next, 0);
      if (*next != '\0')
        errx(1, "-s needs format <min> or <min>:<max>");
      break;
    }
    case 'n':
      filter_ndecoded_same = true;
      break;
    case 'h':
    default:
      fprintf(stderr, "usage: %s [-t] [-m mutator] [-s min[:max]] [-n]\n", argv[0]);
      fprintf(stderr, "  -t: use thread mode\n");
      fprintf(stderr, "  -m: specify mutator\n");
      fprintf(stderr, "  -s: keep samples where success count is in range; default is 1:-1\n");
      fprintf(stderr, "      (0 = all; 1 = #success >= 1; -1 = #success = nworkers - 1;\n");
      fprintf(stderr, "       1:-2 = #success >= 1 && <= nworkers - 1;\n");
      fprintf(stderr, "       1:0 = filter all (e.g., for use with -n); etc.)\n");
      fprintf(stderr, "  -n: keep samples where successful ndecoded differs\n");
      return 1;
    }
  }

  if (optind + 1 != argc) {
    fprintf(stderr, "expected worker file as positional argument\n");
    return 1;
  }

  if (!thread_mode) {
    struct sigaction sigchld_action = {0};
    sigchld_action.sa_handler = sigchld_handler;
    sigchld_action.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sigchld_action, NULL)) {
      perror("sigaction");
      return 1;
    }
  }

  mutator_t mutator = mutator_create(mutator_name);

  FILE *file = fopen(argv[optind], "r");
  if (file == NULL) {
    perror(argv[optind]);
    return 1;
  }

  input_chunk *input_chunks = alloc_shared(sizeof(input_chunk) * MISHEGOS_NUM_CHUNKS);

  int nworkers = 0;
  uint64_t gen = 1;
  uint64_t idx = 0;

  while (nworkers < MISHEGOS_MAX_NWORKERS) {
    size_t size = 0;
    char *line = NULL;
    if (getline(&line, &size, file) < 0 || feof(file) != 0) {
      break;
    }
    if (line[0] == '#') {
      continue;
    }

    /* getline retains the newline if present, so chop it off. */
    line[strcspn(line, "\n")] = '\0';
    if (access(line, R_OK) < 0) {
      perror(line);
      return 1;
    }

    workers[nworkers].soname_len = strlen(line);
    workers[nworkers].soname = line;
    workers[nworkers].workerno = nworkers;
    workers[nworkers].input_chunks = input_chunks;
    workers[nworkers].output_chunks = alloc_shared(sizeof(output_chunk) * MISHEGOS_NUM_CHUNKS);
    workers[nworkers].start_gen = gen;
    workers[nworkers].start_idx = idx;
    worker_start(&workers[nworkers]);
    nworkers++;
  }

  if (filter_min_success < 0) {
    filter_min_success += nworkers + 1;
  }
  if (filter_max_success < 0) {
    filter_max_success += nworkers + 1;
  }
  fprintf(stderr, "filter min=%d max=%d\n", filter_min_success, filter_max_success);

  uint64_t total = 0;
  uint64_t exit_idx = MISHEGOS_NUM_CHUNKS;
  while (true) {
    mish_atomic_wait_for(&input_chunks[idx].remaining_workers, 0);

    if (!thread_mode) {
      bool worker_restarted = false;
      for (int i = 0; i < nworkers; i++) {
        if (workers[i].sigchld) {
          /* undo hack to forcefully wake us up. */
          mish_atomic_fetch_add(&input_chunks[workers[i].start_idx].remaining_workers, 1);
          workers[i].sigchld = false;
          worker_start(&workers[i]);
          worker_restarted = true;
        }
      }
      if (worker_restarted) {
        /* if we restarted a worker for current idx, wait for it again. */
        continue;
      }
    }

    if (gen > 1) {
      for (size_t i = 0; i < input_chunks[idx].input_count; i++) {
        process(idx, i, input_chunks, nworkers, workers);
      }
    }

    if (idx == exit_idx) {
      break;
    }

    // Not yet exiting, so fill another chunk.
    if (exit_idx == MISHEGOS_NUM_CHUNKS) {
      size_t count = 0;
      for (size_t i = 0; i < MISHEGOS_NUM_SLOTS_PER_CHUNK; i++) {
        bool filled = mutator(&input_chunks[idx].inputs[i]);
        if (filled) {
          count++;
        } else { // no more mutations
          exit_idx = idx;
          break;
        }
      }

      input_chunks[idx].input_count = count;
      mish_atomic_store(&input_chunks[idx].remaining_workers, nworkers);
      mish_atomic_store(&input_chunks[idx].generation, gen);
      mish_atomic_notify(&input_chunks[idx].generation);
    }

    idx++;
    if (idx == MISHEGOS_NUM_CHUNKS) {
      idx = 0;
      gen++;
    }
  }
}
