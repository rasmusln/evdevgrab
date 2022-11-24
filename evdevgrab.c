#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <argp.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <libevdev/libevdev.h>

#define MAX_EVENTS 10

const char *argp_program_version = "0.0.1";
const char *argp_program_bug_address = "/dev/null";

static char doc[] = "Grab evdev device(s) and print events to stdout. DEVICE is the path to an evdev device.\vExamples:\n\tevdevgrab /dev/input/event2 /dev/input/event3\n\tevdevgrab -v -n /dev/input/evdevgrab";
static char args_doc[] = "[DEVICE]..";

static struct argp_option options[] = {
	{ "verbose", 'v', 0, 0, "Produce verbose output" },
	{ "no-grab", 'n', 0, 0, "No grab of devices" },
	{ 0 }
};

struct arguments {
	int verbose;
	int no_grab;
	struct Device *head;
};

struct Device {
	char *device_path;
	int device_fd;
	struct libevdev *device;
	struct Device *next;
};

bool is_valid(struct Device *device) {
	struct stat st;

	if (stat(device->device_path, &st) < 0 && S_ISREG(st.st_mode) == false) {
		return false; 
	} else {
		return true;
	}
}

bool is_readable(struct Device *device) {
	return access(device->device_path, R_OK) < 0 ? false : true;
}

void free_device(struct Device *device) {
	if (device->device_path != NULL) {
		free(device->device_path);
		device->device_path = NULL;
	}

	if (device->device != NULL) {
		libevdev_free(device->device);
		device->device = NULL;
	}

	if (device->device_fd != -1) {
		close(device->device_fd);
		device->device_fd = -1;
	}
}

int create(struct Device **device, char *device_path) {
	struct Device *d;

	d = malloc(sizeof(struct Device));
	if (d == NULL) {
		return -1;
	}

	d->device_path = malloc(strlen(device_path+1));
	if (d->device_path == NULL) {
		return -1;
	}

	strcpy(d->device_path, device_path);

	d->device_fd = 0;
	d->device = NULL;
	d->next = NULL;

	(*device) = d;

	return 0;
}

void append(struct Device **head, struct Device *device) {
	struct Device *d, *p;

	p = NULL;
	d = *head;

	while (d != NULL) {
		p = d;
		d = d->next;
	}

	d = device;

	if (p != NULL) {
		p->next = d;
	}

	if (*head == NULL) {
		*head = d;
	}
}

void free_devices(struct Device *head) {
	struct Device *d;

	if (head != NULL) {
		do {
			d = head;
			head =  head->next;

			free_device(d);
		} while (head != NULL);
	}
}

int epoll_add(int epfd, int fd, void *ptr) {
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.fd = fd;

	// note that ev.data is an union and this statement effectively overwrite
	if (ptr != NULL) {
		ev.data.ptr = ptr;
	}

	return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int initialize(struct Device *device, int epfd, bool grab) {
	int rc;

	device->device_fd = open(device->device_path, O_RDONLY|O_NONBLOCK);
	if (device->device_fd < 0) {
		fprintf(stderr, "failed to initialize file descriptor for: %s\n", device->device_path);
		return -1;
	}

	rc = libevdev_new_from_fd(device->device_fd, &(device->device));
	if (rc < 0) {
		fprintf(stderr, "failed to initialize evdev device for path: %s\n", device->device_path);
		return rc;
	}

	rc = epoll_add(epfd, device->device_fd, (void *)device);
	if (rc < 0) {
		fprintf(stderr, "failed to add to epoll for path: %s\n", device->device_path);
		return rc;
	}

	if (grab) {
		rc = libevdev_grab(device->device, LIBEVDEV_GRAB);
		if (rc < 0) {
			fprintf(stderr, "failed to grab device for path: %s\n", device-> device_path);
			return rc;
		}
	}

	return 0;
}

int next_event(struct Device *device, bool verbose) {
	int rc = 0;
	struct input_event ev;

	while (!(rc < 0)) {
		rc = libevdev_next_event(device->device, LIBEVDEV_READ_FLAG_NORMAL, &ev);

		switch (rc) {
			case LIBEVDEV_READ_STATUS_SUCCESS:
				if (verbose) {
					printf("next event -> status success\n");
				}
				break;
			case LIBEVDEV_READ_STATUS_SYNC:
				if (verbose) {
					printf("next event -> status sync\n");
				}
				break;
			case -EAGAIN:
				break;
		}

		printf("event: %s %s %d\n",
			libevdev_event_type_get_name(ev.type),
			libevdev_event_code_get_name(ev.type, ev.code),
			ev.value);
	}

	return 0;
}

int block_signals(int epfd) {
	sigset_t mask;
	int rc, signal_fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd == -1) {
		fprintf(stderr, "failed to create signal file descriptor");
		return -1;
	}

	rc = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to create sigprocmask");
		close(signal_fd);
		return rc;
	}

	rc = epoll_add(epfd, signal_fd, NULL);
	if (rc < 0) {
		fprintf(stderr, "failed to add signal file descriptor to epoll");
		close(signal_fd);
		return rc;
	}

	return signal_fd;
}

void cleanup(struct Device *head, int *epfd, int *signal_fd) {
	free_devices(head);

	if (!(*epfd < 0)) {
		close(*epfd);
		*epfd = -1;
	}

	if (!(*signal_fd < 0)) {
		close(*signal_fd);
		*signal_fd = -1;
	}
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	int rc, epfd = -1, signal_fd = -1;
	struct Device *d;

	struct arguments *arguments = state->input;

	switch (key) {
		case 'v':
			arguments->verbose = true;
			break;
		case 'n':
			arguments->no_grab = true;
			break;
		case ARGP_KEY_ARG:


			rc = create(&d, arg);
			if (rc < 0) {
				return ARGP_HELP_STD_USAGE;
			}

			if (!is_valid(d)) {
				fprintf(stderr, "%s is not a valid device\n", arg);
				cleanup(arguments->head, &epfd, &signal_fd);
				return ARGP_HELP_STD_ERR;
			}

			if (!is_readable(d)) {
				fprintf(stderr, "%s is not readable\n", arg);
				cleanup(arguments->head, &epfd, &signal_fd);
				return ARGP_HELP_STD_ERR;
			}

			append(&(arguments->head), d);

			break;
	}

	return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc };

int main(int argc, char **argv) {
	int rc, epfd = -1, signal_fd = -1, nfds = -1;;
	struct epoll_event events[MAX_EVENTS];
	struct Device *d;
	struct arguments arguments;

	arguments.head = NULL;

	argp_parse(&argp, argc, argv, 0, 0, &arguments);

	epfd = epoll_create1(0);
	if (epfd < 0) {
		fprintf(stderr, "failed to create epoll");
		cleanup((arguments.head), &epfd, &signal_fd);
		exit(1);
	}

	if (arguments.head != NULL) {
		for (d = arguments.head; d != NULL; d = d->next) {
			if (arguments.verbose) {
				printf("Device at path: %s\n", d->device_path);
			}

			rc = initialize(d, epfd, !arguments.no_grab);
			if (rc < 0) {
				break;
			}
		}
		if (rc < 0) {
			cleanup((arguments.head), &epfd, &signal_fd);
			exit(1);
		}

		signal_fd = block_signals(epfd);
		if (signal_fd < 0) {
			fprintf(stderr, "failed to adapt interrupt signal to epoll");
			cleanup((arguments.head), &epfd, &signal_fd);
			exit(1);
		}

		while (true) {
			nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

			if (nfds == -1) {
				fprintf(stderr, "epoll file descriptor failure");
				cleanup((arguments.head), &epfd, &signal_fd);
				exit(1);
			}

			for (int n = 0; n < nfds; n++) {
				if (events[n].data.fd == signal_fd) {
					cleanup((arguments.head), &epfd, &signal_fd);
					exit(1);
				}

				if (events[n].data.ptr != NULL) {
					d = (struct Device *) events[n].data.ptr;
					
					rc = next_event(d, arguments.verbose);
					if (rc < 0) {
						fprintf(stderr, "next_efent failed with code: %d\n", rc);
					}
				}
			}
		}

		cleanup((arguments.head), &epfd, &signal_fd);
	}
}

