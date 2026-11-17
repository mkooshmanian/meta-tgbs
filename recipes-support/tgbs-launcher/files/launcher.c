/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "launcher.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

static int error(const char *operation)
{
	fprintf(stderr, "tgbs-launcher: %s: %s\n", operation, strerror(errno));
	return 1;
}

static int signals_begin(void)
{
	sigset_t mask;
	struct sigaction action = { .sa_handler = SIG_DFL };
	const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
	sigemptyset(&action.sa_mask);
	/* libtgbs owns/reaps the container child, including when the launcher
	 * inherited SIGCHLD=SIG_IGN from its caller. */
	if (sigaction(SIGCHLD, &action, NULL) != 0)
		return -1;
	sigemptyset(&mask);
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
		sigaddset(&mask, signals[i]);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
		return -1;
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
		if (sigaction(signals[i], &action, NULL) != 0)
			return -1;
	/* Each worker has its own cancellation descriptor. No parent lifetime
	 * or other domain's exit can trigger it. */
	return signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
}

static int run_domain(tgbs_domain_spec_t spec)
{
	tgbs_exit_status_t status;
	int rc = tgbs_domain_run(&spec, &status);
	int saved = errno;
	if (rc != 0)
		fprintf(stderr, "tgbs-launcher: domain %s: %s\n", spec.name, strerror(saved));
	if (status.cleanup_errno)
		fprintf(stderr, "tgbs-launcher: domain %s cleanup: %s\n",
			spec.name, strerror(status.cleanup_errno));
	if (rc != 0 || status.cleanup_errno)
		return 1;
	if (status.cancelled) {
		fprintf(stderr, "tgbs-launcher: domain %s stopped and cleaned up\n", spec.name);
		return 0;
	}
	if (status.exited) {
		fprintf(stderr, "tgbs-launcher: domain %s exited with code %d\n", spec.name, status.exit_code);
		return status.exit_code ? 1 : 0;
	}
	fprintf(stderr, "tgbs-launcher: domain %s terminated by signal %d\n", spec.name, status.term_signal);
	return 1;
}

static int launch(struct launcher_config *config, int config_fd)
{
	size_t created = 0, launched = 0;
	int result = 0;
	for (size_t i = 0; i < config->channel_count; i++) {
		if (tgbs_channel_create(&config->channels[i]) != 0) {
			fprintf(stderr, "tgbs-launcher: create channel %s: %s\n",
				config->channels[i].name, strerror(errno));
			result = 1;
			goto rollback;
		}
		created++;
	}
	for (size_t i = 0; i < config->domain_count; i++) {
		fflush(NULL);
		pid_t pid = fork();
		if (pid < 0) {
			result = error("fork worker");
			continue;
		}
		if (pid == 0) {
			close(config_fd);
			if (setsid() < 0)
				_exit(error("detach worker session"));
			int signal_fd = signals_begin();
			if (signal_fd < 0)
				_exit(error("prepare worker signals"));
			tgbs_domain_spec_t spec = config->domains[i];
			spec.cancel_fd = signal_fd;
			/* Linux comm allows 15 visible bytes; keep the -run suffix. */
			char process_name[16];
			snprintf(process_name, sizeof(process_name), "%.11s-run", spec.name);
			if (prctl(PR_SET_NAME, process_name, 0L, 0L, 0L) != 0)
				error("name worker");
			int rc = run_domain(spec);
			close(signal_fd);
			_exit(rc);
		}
		launched++;
		fprintf(stderr, "tgbs-launcher: launching domain %s (worker %ld)\n",
			config->domains[i].name, (long)pid);
	}
	/* Workers are adopted by init after this parent exits. They retain the
	 * caller's stdio and own their domain's blocking run through cleanup.
	 * Channels must remain available for them and for later tgbsctl runs. */
	if (launched)
		return result;
rollback:
	/* No worker was launched, so only our newly created channels are removed. */
	while (created) {
		const char *name = config->channels[--created].name;
		if (tgbs_channel_delete(name) != 0) {
			fprintf(stderr, "tgbs-launcher: delete channel %s: %s\n", name, strerror(errno));
			result = 1;
		}
	}
	return result;
}

int main(int argc, char **argv)
{
	int check = argc == 3 && !strcmp(argv[1], "--check");
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		printf("Usage: tgbs-launcher [--check] CONFIG.json\n"
			"Launch all configured domains in the background, then exit.\n"
			"Success reports forks only; domain startup errors go to inherited stderr.\n"
			"--check validates configuration without creating runtime resources.\n");
		return 0;
	}
	if ((!check && argc != 2) || (!check && argv[1][0] == '-')) {
		fprintf(stderr, "Usage: tgbs-launcher [--check] CONFIG.json\n");
		return 2;
	}
	const char *path = argv[check ? 2 : 1];
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return error(path);
	struct stat st;
	int result = 1;
	struct launcher_config config = {0};
	if (fstat(fd, &st) != 0) {
		error("stat configuration");
		goto out;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "tgbs-launcher: configuration must be a regular file\n");
		goto out;
	}
	/* Advisory, inode-based lock during launch only. Workers close this fd;
	 * libtgbs independently enforces exclusive ownership of each domain. */
	if (!check && flock(fd, LOCK_EX | LOCK_NB) != 0) {
		error("lock configuration (another launcher may be using it)");
		goto out;
	}
	if (launcher_config_load(fd, &config) != 0) {
		result = 2;
		goto out;
	}
	if (check) {
		printf("tgbs-launcher: configuration valid (%zu domains, %zu channels)\n",
			config.domain_count, config.channel_count);
		result = 0;
	} else {
		result = launch(&config, fd);
	}
out:
	launcher_config_free(&config);
	close(fd);
	return result;
}
