// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/utsname.h>
#include <perf/cpumap.h>
#include <perf/evlist.h>
#include <perf/mmap.h>

#include "debug.h"
#include "dso.h"
#include "env.h"
#include "parse-events.h"
#include "evlist.h"
#include "evsel.h"
#include "thread_map.h"
#include "machine.h"
#include "map.h"
#include "symbol.h"
#include "event.h"
#include "record.h"
#include "util/mmap.h"
#include "util/string2.h"
#include "util/synthetic-events.h"
#include "util/util.h"
#include "thread.h"

#include "tests.h"

#include <linux/ctype.h>

#define BUFSZ	1024
#define READLEN	128

struct state {
	u64 done[1024];
	size_t done_cnt;
};

static size_t read_objdump_chunk(const char **line, unsigned char **buf,
				 size_t *buf_len)
{
	size_t bytes_read = 0;
	unsigned char *chunk_start = *buf;

	/* Read bytes */
	while (*buf_len > 0) {
		char c1, c2;

		/* Get 2 hex digits */
		c1 = *(*line)++;
		if (!isxdigit(c1))
			break;
		c2 = *(*line)++;
		if (!isxdigit(c2))
			break;

		/* Store byte and advance buf */
		**buf = (hex(c1) << 4) | hex(c2);
		(*buf)++;
		(*buf_len)--;
		bytes_read++;

		/* End of chunk? */
		if (isspace(**line))
			break;
	}

	/*
	 * objdump will display raw insn as LE if code endian
	 * is LE and bytes_per_chunk > 1. In that case reverse
	 * the chunk we just read.
	 *
	 * see disassemble_bytes() at binutils/objdump.c for details
	 * how objdump chooses display endian)
	 */
	if (bytes_read > 1 && !host_is_bigendian()) {
		unsigned char *chunk_end = chunk_start + bytes_read - 1;
		unsigned char tmp;

		while (chunk_start < chunk_end) {
			tmp = *chunk_start;
			*chunk_start = *chunk_end;
			*chunk_end = tmp;
			chunk_start++;
			chunk_end--;
		}
	}

	return bytes_read;
}

static size_t read_objdump_line(const char *line, unsigned char *buf,
				size_t buf_len)
{
	const char *p;
	size_t ret, bytes_read = 0;

	/* Skip to a colon */
	p = strchr(line, ':');
	if (!p)
		return 0;
	p++;

	/* Skip initial spaces */
	while (*p) {
		if (!isspace(*p))
			break;
		p++;
	}

	do {
		ret = read_objdump_chunk(&p, &buf, &buf_len);
		bytes_read += ret;
		p++;
	} while (ret > 0);

	/* return number of successfully read bytes */
	return bytes_read;
}

static int read_objdump_output(FILE *f, void *buf, size_t *len, u64 start_addr)
{
	char *line = NULL;
	size_t line_len, off_last = 0;
	ssize_t ret;
	int err = 0;
	u64 addr, last_addr = start_addr;

	while (off_last < *len) {
		size_t off, read_bytes, written_bytes;
		unsigned char tmp[BUFSZ];

		ret = getline(&line, &line_len, f);
		if (feof(f))
			break;
		if (ret < 0) {
			pr_debug("getline failed\n");
			err = -1;
			break;
		}

		/* read objdump data into temporary buffer */
		read_bytes = read_objdump_line(line, tmp, sizeof(tmp));
		if (!read_bytes)
			continue;

		if (sscanf(line, "%"PRIx64, &addr) != 1)
			continue;
		if (addr < last_addr) {
			pr_debug("addr going backwards, read beyond section?\n");
			break;
		}
		last_addr = addr;

		/* copy it from temporary buffer to 'buf' according
		 * to address on current objdump line */
		off = addr - start_addr;
		if (off >= *len)
			break;
		written_bytes = MIN(read_bytes, *len - off);
		memcpy(buf + off, tmp, written_bytes);
		off_last = off + written_bytes;
	}

	/* len returns number of bytes that could not be read */
	*len -= off_last;

	free(line);

	return err;
}

/*
 * Only gets GNU objdump version. Returns 0 for llvm-objdump.
 */
static int objdump_version(void)
{
	size_t line_len;
	char cmd[PATH_MAX * 2];
	char *line = NULL;
	const char *fmt;
	FILE *f;
	int ret;

	int version_tmp, version_num = 0;
	char *version = 0, *token;

	fmt = "%s --version";
	ret = snprintf(cmd, sizeof(cmd), fmt, test_objdump_path);
	if (ret <= 0 || (size_t)ret >= sizeof(cmd))
		return -1;
	/* Ignore objdump errors */
	strcat(cmd, " 2>/dev/null");
	f = popen(cmd, "r");
	if (!f) {
		pr_debug("popen failed\n");
		return -1;
	}
	/* Get first line of objdump --version output */
	ret = getline(&line, &line_len, f);
	pclose(f);
	if (ret < 0) {
		pr_debug("getline failed\n");
		return -1;
	}

	token = strsep(&line, " ");
	if (token != NULL && !strcmp(token, "GNU")) {
		// version is last part of first line of objdump --version output.
		while ((token = strsep(&line, " ")))
			version = token;

		// Convert version into a format we can compare with
		token = strsep(&version, ".");
		version_num = atoi(token);
		if (version_num)
			version_num *= 10000;

		token = strsep(&version, ".");
		version_tmp = atoi(token);
		if (token)
			version_num += version_tmp * 100;

		token = strsep(&version, ".");
		version_tmp = atoi(token);
		if (token)
			version_num += version_tmp;
	}

	return version_num;
}

static int read_via_objdump(const char *filename, u64 addr, void *buf,
			    size_t len)
{
	u64 stop_address = addr + len;
	struct utsname uname_buf;
	char cmd[PATH_MAX * 2];
	const char *fmt;
	FILE *f;
	int ret;

	ret = uname(&uname_buf);
	if (ret) {
		pr_debug("uname failed\n");
		return -1;
	}

	if (!strncmp(uname_buf.machine, "riscv", 5)) {
		int version = objdump_version();

		/* Default to this workaround if version parsing fails */
		if (version < 0 || version > 24100) {
			/*
			 * Starting at riscv objdump version 2.41, dumping in
			 * the middle of an instruction is not supported. riscv
			 * instructions are aligned along 2-byte intervals and
			 * can be either 2-bytes or 4-bytes. This makes it
			 * possible that the stop-address lands in the middle of
			 * a 4-byte instruction. Increase the stop_address by
			 * two to ensure an instruction is not cut in half, but
			 * leave the len as-is so only the expected number of
			 * bytes are collected.
			 */
			stop_address += 2;
		}
	}

	fmt = "%s -z -d --start-address=0x%"PRIx64" --stop-address=0x%"PRIx64" %s";
	ret = snprintf(cmd, sizeof(cmd), fmt, test_objdump_path, addr, stop_address,
		       filename);
	if (ret <= 0 || (size_t)ret >= sizeof(cmd))
		return -1;

	pr_debug("Objdump command is: %s\n", cmd);

	/* Ignore objdump errors */
	strcat(cmd, " 2>/dev/null");

	f = popen(cmd, "r");
	if (!f) {
		pr_debug("popen failed\n");
		return -1;
	}

	ret = read_objdump_output(f, buf, &len, addr);
	if (len) {
		pr_debug("objdump read too few bytes: %zd\n", len);
		if (!ret)
			ret = len;
	}

	pclose(f);

	return ret;
}

static void dump_buf(unsigned char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		pr_debug("0x%02x ", buf[i]);
		if (i % 16 == 15)
			pr_debug("\n");
	}
	pr_debug("\n");
}

static int read_object_code(u64 addr, size_t len, u8 cpumode,
			    struct thread *thread, struct state *state)
{
	struct addr_location al;
	unsigned char buf1[BUFSZ] = {0};
	unsigned char buf2[BUFSZ] = {0};
	size_t ret_len;
	u64 objdump_addr;
	const char *objdump_name;
	char decomp_name[KMOD_DECOMP_LEN];
	bool decomp = false;
	int ret, err = 0;
	struct dso *dso;

	pr_debug("Reading object code for memory address: %#"PRIx64"\n", addr);

	addr_location__init(&al);
	if (!thread__find_map(thread, cpumode, addr, &al) || !map__dso(al.map)) {
		if (cpumode == PERF_RECORD_MISC_HYPERVISOR) {
			pr_debug("Hypervisor address can not be resolved - skipping\n");
			goto out;
		}

		pr_debug("thread__find_map failed\n");
		err = -1;
		goto out;
	}
	dso = map__dso(al.map);
	pr_debug("File is: %s\n", dso__long_name(dso));

	if (dso__symtab_type(dso) == DSO_BINARY_TYPE__KALLSYMS && !dso__is_kcore(dso)) {
		pr_debug("Unexpected kernel address - skipping\n");
		goto out;
	}

	pr_debug("On file address is: %#"PRIx64"\n", al.addr);

	if (len > BUFSZ)
		len = BUFSZ;

	/* Do not go off the map */
	if (addr + len > map__end(al.map))
		len = map__end(al.map) - addr;

	/*
	 * Some architectures (ex: powerpc) have stubs (trampolines) in kernel
	 * modules to manage long jumps. Check if the ip offset falls in stubs
	 * sections for kernel modules. And skip module address after text end
	 */
	if (dso__is_kmod(dso) && al.addr > dso__text_end(dso)) {
		pr_debug("skipping the module address %#"PRIx64" after text end\n", al.addr);
		goto out;
	}

	/* Read the object code using perf */
	ret_len = dso__data_read_offset(dso, maps__machine(thread__maps(thread)),
					al.addr, buf1, len);
	if (ret_len != len) {
		pr_debug("dso__data_read_offset failed\n");
		err = -1;
		goto out;
	}

	/*
	 * Converting addresses for use by objdump requires more information.
	 * map__load() does that.  See map__rip_2objdump() for details.
	 */
	if (map__load(al.map)) {
		err = -1;
		goto out;
	}

	/* objdump struggles with kcore - try each map only once */
	if (dso__is_kcore(dso)) {
		size_t d;

		for (d = 0; d < state->done_cnt; d++) {
			if (state->done[d] == map__start(al.map)) {
				pr_debug("kcore map tested already");
				pr_debug(" - skipping\n");
				goto out;
			}
		}
		if (state->done_cnt >= ARRAY_SIZE(state->done)) {
			pr_debug("Too many kcore maps - skipping\n");
			goto out;
		}
		state->done[state->done_cnt++] = map__start(al.map);
	}

	objdump_name = dso__long_name(dso);
	if (dso__needs_decompress(dso)) {
		if (dso__decompress_kmodule_path(dso, objdump_name,
						 decomp_name,
						 sizeof(decomp_name)) < 0) {
			pr_debug("decompression failed\n");
			err = -1;
			goto out;
		}

		decomp = true;
		objdump_name = decomp_name;
	}

	/* Read the object code using objdump */
	objdump_addr = map__rip_2objdump(al.map, al.addr);
	ret = read_via_objdump(objdump_name, objdump_addr, buf2, len);

	if (decomp)
		unlink(objdump_name);

	if (ret > 0) {
		/*
		 * The kernel maps are inaccurate - assume objdump is right in
		 * that case.
		 */
		if (cpumode == PERF_RECORD_MISC_KERNEL ||
		    cpumode == PERF_RECORD_MISC_GUEST_KERNEL) {
			len -= ret;
			if (len) {
				pr_debug("Reducing len to %zu\n", len);
			} else if (dso__is_kcore(dso)) {
				/*
				 * objdump cannot handle very large segments
				 * that may be found in kcore.
				 */
				pr_debug("objdump failed for kcore");
				pr_debug(" - skipping\n");
			} else {
				err = -1;
			}
			goto out;
		}
	}
	if (ret < 0) {
		pr_debug("read_via_objdump failed\n");
		err = -1;
		goto out;
	}

	/* The results should be identical */
	if (memcmp(buf1, buf2, len)) {
		pr_debug("Bytes read differ from those read by objdump\n");
		pr_debug("buf1 (dso):\n");
		dump_buf(buf1, len);
		pr_debug("buf2 (objdump):\n");
		dump_buf(buf2, len);
		err = -1;
		goto out;
	}
	pr_debug("Bytes read match those read by objdump\n");
out:
	addr_location__exit(&al);
	return err;
}

static int process_sample_event(struct machine *machine,
				struct evlist *evlist,
				union perf_event *event, struct state *state)
{
	struct perf_sample sample;
	struct thread *thread;
	int ret;

	perf_sample__init(&sample, /*all=*/false);
	ret = evlist__parse_sample(evlist, event, &sample);
	if (ret) {
		pr_debug("evlist__parse_sample failed\n");
		ret = -1;
		goto out;
	}

	thread = machine__findnew_thread(machine, sample.pid, sample.tid);
	if (!thread) {
		pr_debug("machine__findnew_thread failed\n");
		ret = -1;
		goto out;
	}

	ret = read_object_code(sample.ip, READLEN, sample.cpumode, thread, state);
	thread__put(thread);
out:
	perf_sample__exit(&sample);
	return ret;
}

static int process_event(struct machine *machine, struct evlist *evlist,
			 union perf_event *event, struct state *state)
{
	if (event->header.type == PERF_RECORD_SAMPLE)
		return process_sample_event(machine, evlist, event, state);

	if (event->header.type == PERF_RECORD_THROTTLE ||
	    event->header.type == PERF_RECORD_UNTHROTTLE)
		return 0;

	if (event->header.type < PERF_RECORD_MAX) {
		int ret;

		ret = machine__process_event(machine, event, NULL);
		if (ret < 0)
			pr_debug("machine__process_event failed, event type %u\n",
				 event->header.type);
		return ret;
	}

	return 0;
}

static int process_events(struct machine *machine, struct evlist *evlist,
			  struct state *state)
{
	union perf_event *event;
	struct mmap *md;
	int i, ret;

	for (i = 0; i < evlist->core.nr_mmaps; i++) {
		md = &evlist->mmap[i];
		if (perf_mmap__read_init(&md->core) < 0)
			continue;

		while ((event = perf_mmap__read_event(&md->core)) != NULL) {
			ret = process_event(machine, evlist, event, state);
			perf_mmap__consume(&md->core);
			if (ret < 0)
				return ret;
		}
		perf_mmap__read_done(&md->core);
	}
	return 0;
}

static int comp(const void *a, const void *b)
{
	return *(int *)a - *(int *)b;
}

static void do_sort_something(void)
{
	int buf[40960], i;

	for (i = 0; i < (int)ARRAY_SIZE(buf); i++)
		buf[i] = ARRAY_SIZE(buf) - i - 1;

	qsort(buf, ARRAY_SIZE(buf), sizeof(int), comp);

	for (i = 0; i < (int)ARRAY_SIZE(buf); i++) {
		if (buf[i] != i) {
			pr_debug("qsort failed\n");
			break;
		}
	}
}

static void sort_something(void)
{
	int i;

	for (i = 0; i < 10; i++)
		do_sort_something();
}

static void syscall_something(void)
{
	int pipefd[2];
	int i;

	for (i = 0; i < 1000; i++) {
		if (pipe(pipefd) < 0) {
			pr_debug("pipe failed\n");
			break;
		}
		close(pipefd[1]);
		close(pipefd[0]);
	}
}

static void fs_something(void)
{
	const char *test_file_name = "temp-perf-code-reading-test-file--";
	FILE *f;
	int i;

	for (i = 0; i < 1000; i++) {
		f = fopen(test_file_name, "w+");
		if (f) {
			fclose(f);
			unlink(test_file_name);
		}
	}
}

static void do_something(void)
{
	fs_something();

	sort_something();

	syscall_something();
}

enum {
	TEST_CODE_READING_OK,
	TEST_CODE_READING_NO_VMLINUX,
	TEST_CODE_READING_NO_KCORE,
	TEST_CODE_READING_NO_ACCESS,
	TEST_CODE_READING_NO_KERNEL_OBJ,
};

static int do_test_code_reading(bool try_kcore)
{
	struct machine *machine;
	struct thread *thread;
	struct record_opts opts = {
		.mmap_pages	     = UINT_MAX,
		.user_freq	     = UINT_MAX,
		.user_interval	     = ULLONG_MAX,
		.freq		     = 500,
		.target		     = {
			.uses_mmap   = true,
		},
	};
	struct state state = {
		.done_cnt = 0,
	};
	struct perf_thread_map *threads = NULL;
	struct perf_cpu_map *cpus = NULL;
	struct evlist *evlist = NULL;
	struct evsel *evsel = NULL;
	int err = -1, ret;
	pid_t pid;
	struct map *map;
	bool have_vmlinux, have_kcore;
	struct dso *dso;
	const char *events[] = { "cycles", "cycles:u", "cpu-clock", "cpu-clock:u", NULL };
	int evidx = 0;
	struct perf_env host_env;

	pid = getpid();

	perf_env__init(&host_env);
	machine = machine__new_host(&host_env);

	ret = machine__create_kernel_maps(machine);
	if (ret < 0) {
		pr_debug("machine__create_kernel_maps failed\n");
		goto out_err;
	}

	/* Force the use of kallsyms instead of vmlinux to try kcore */
	if (try_kcore)
		symbol_conf.kallsyms_name = "/proc/kallsyms";

	/* Load kernel map */
	map = machine__kernel_map(machine);
	ret = map__load(map);
	if (ret < 0) {
		pr_debug("map__load failed\n");
		goto out_err;
	}
	dso = map__dso(map);
	have_vmlinux = dso__is_vmlinux(dso);
	have_kcore = dso__is_kcore(dso);

	/* 2nd time through we just try kcore */
	if (try_kcore && !have_kcore)
		return TEST_CODE_READING_NO_KCORE;

	/* No point getting kernel events if there is no kernel object */
	if (!have_vmlinux && !have_kcore)
		evidx++;

	threads = thread_map__new_by_tid(pid);
	if (!threads) {
		pr_debug("thread_map__new_by_tid failed\n");
		goto out_err;
	}

	ret = perf_event__synthesize_thread_map(NULL, threads,
						perf_event__process, machine,
						true, false);
	if (ret < 0) {
		pr_debug("perf_event__synthesize_thread_map failed\n");
		goto out_err;
	}

	thread = machine__findnew_thread(machine, pid, pid);
	if (!thread) {
		pr_debug("machine__findnew_thread failed\n");
		goto out_put;
	}

	cpus = perf_cpu_map__new_online_cpus();
	if (!cpus) {
		pr_debug("perf_cpu_map__new failed\n");
		goto out_put;
	}

	while (events[evidx]) {
		const char *str;

		evlist = evlist__new();
		if (!evlist) {
			pr_debug("evlist__new failed\n");
			goto out_put;
		}

		perf_evlist__set_maps(&evlist->core, cpus, threads);

		str = events[evidx];
		pr_debug("Parsing event '%s'\n", str);
		ret = parse_event(evlist, str);
		if (ret < 0) {
			pr_debug("parse_events failed\n");
			goto out_put;
		}

		evlist__config(evlist, &opts, NULL);

		evlist__for_each_entry(evlist, evsel) {
			evsel->core.attr.comm = 1;
			evsel->core.attr.disabled = 1;
			evsel->core.attr.enable_on_exec = 0;
		}

		ret = evlist__open(evlist);
		if (ret < 0) {
			evidx++;

			if (events[evidx] == NULL && verbose > 0) {
				char errbuf[512];
				evlist__strerror_open(evlist, errno, errbuf, sizeof(errbuf));
				pr_debug("perf_evlist__open() failed!\n%s\n", errbuf);
			}

			perf_evlist__set_maps(&evlist->core, NULL, NULL);
			evlist__delete(evlist);
			evlist = NULL;
			continue;
		}
		break;
	}

	if (events[evidx] == NULL)
		goto out_put;

	ret = evlist__mmap(evlist, UINT_MAX);
	if (ret < 0) {
		pr_debug("evlist__mmap failed\n");
		goto out_put;
	}

	evlist__enable(evlist);

	do_something();

	evlist__disable(evlist);

	ret = process_events(machine, evlist, &state);
	if (ret < 0)
		goto out_put;

	if (!have_vmlinux && !have_kcore && !try_kcore)
		err = TEST_CODE_READING_NO_KERNEL_OBJ;
	else if (!have_vmlinux && !try_kcore)
		err = TEST_CODE_READING_NO_VMLINUX;
	else if (strstr(events[evidx], ":u"))
		err = TEST_CODE_READING_NO_ACCESS;
	else
		err = TEST_CODE_READING_OK;
out_put:
	thread__put(thread);
out_err:
	evlist__delete(evlist);
	perf_cpu_map__put(cpus);
	perf_thread_map__put(threads);
	machine__delete(machine);
	perf_env__exit(&host_env);

	return err;
}

static int test__code_reading(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	int ret;

	ret = do_test_code_reading(false);
	if (!ret)
		ret = do_test_code_reading(true);

	switch (ret) {
	case TEST_CODE_READING_OK:
		return 0;
	case TEST_CODE_READING_NO_VMLINUX:
		pr_debug("no vmlinux\n");
		return 0;
	case TEST_CODE_READING_NO_KCORE:
		pr_debug("no kcore\n");
		return 0;
	case TEST_CODE_READING_NO_ACCESS:
		pr_debug("no access\n");
		return 0;
	case TEST_CODE_READING_NO_KERNEL_OBJ:
		pr_debug("no kernel obj\n");
		return 0;
	default:
		return -1;
	};
}

DEFINE_SUITE("Object code reading", code_reading);
