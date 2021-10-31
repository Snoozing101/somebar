// somebar - dwl bar
// See LICENSE file for copyright and license details.

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <list>
#include <optional>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "common.hpp"
#include "config.hpp"
#include "bar.hpp"
#include "line_buffer.hpp"

struct Monitor {
	uint32_t registryName;
	std::string xdgName;
	wl_unique_ptr<wl_output> wlOutput;
	std::optional<Bar> bar;
	bool desiredVisibility {true};
	bool hasData;
	uint32_t tags;
};

struct SeatPointer {
	wl_unique_ptr<wl_pointer> wlPointer;
	Bar* focusedBar;
	int x, y;
	std::vector<int> btns;
};
struct Seat {
	uint32_t name;
	wl_unique_ptr<wl_seat> wlSeat;
	std::optional<SeatPointer> pointer;
};

static Bar* barFromSurface(const wl_surface* surface);
static void setupMonitor(Monitor& monitor);
static void updatemon(Monitor &mon);
static void onReady();
static void setupStatusFifo();
static void onStatus();
static void onStdin();
static void handleStdin(const std::string& line);
static void updateVisibility(const std::string& name, bool(*updater)(bool));
static void onGlobalAdd(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
static void onGlobalRemove(void*, wl_registry* registry, uint32_t name);
static void requireGlobal(const void* p, const char* name);
static void waylandFlush();
static void cleanup();
[[noreturn]] static void diesys(const char* why);

wl_display* display;
wl_compositor* compositor;
wl_shm* shm;
zwlr_layer_shell_v1* wlrLayerShell;
static xdg_wm_base* xdgWmBase;
static zxdg_output_manager_v1* xdgOutputManager;
static wl_surface* cursorSurface;
static wl_cursor_image* cursorImage;
static bool ready;
static std::list<Monitor> monitors;
static std::list<Seat> seats;
static Monitor* selmon;
static std::string lastStatus;
static std::string statusFifoName;
static int epoll {-1};
static int displayFd {-1};
static int statusFifoFd {-1};
static int statusFifoWriter {-1};
static bool quitting {false};

void spawn(Monitor&, const Arg& arg)
{
	if (fork() == 0) {
		auto argv = static_cast<char* const*>(arg.v);
		setsid();
		execvp(argv[0], argv);
		fprintf(stderr, "somebar: execvp %s ", argv[0]);
		perror(" failed");
		exit(1);
	}
}

static const struct xdg_wm_base_listener xdgWmBaseListener = {
	[](void*, xdg_wm_base* sender, uint32_t serial) {
		xdg_wm_base_pong(sender, serial);
	}
};

static const struct zxdg_output_v1_listener xdgOutputListener = {
	.logical_position = [](void*, zxdg_output_v1*, int, int) { },
	.logical_size = [](void*, zxdg_output_v1*, int, int) { },
	.done = [](void*, zxdg_output_v1*) { },
	.name = [](void* mp, zxdg_output_v1* xdgOutput, const char* name) {
		auto& monitor = *static_cast<Monitor*>(mp);
		monitor.xdgName = name;
		zxdg_output_v1_destroy(xdgOutput);
	},
	.description = [](void*, zxdg_output_v1*, const char*) { },
};

Bar* barFromSurface(const wl_surface* surface)
{
	auto mon = std::find_if(begin(monitors), end(monitors), [surface](const Monitor& mon) {
		return mon.bar && mon.bar->surface() == surface;
	});
	return mon != end(monitors) && mon->bar ? &*mon->bar : nullptr;
}
static const struct wl_pointer_listener pointerListener = {
	.enter = [](void* sp, wl_pointer* pointer, uint32_t serial,
	wl_surface* surface, wl_fixed_t x, wl_fixed_t y)
	{
		auto& seat = *static_cast<Seat*>(sp);
		seat.pointer->focusedBar = barFromSurface(surface);
		if (!cursorImage) {
			auto cursorTheme = wl_cursor_theme_load(nullptr, 24, shm);
			cursorImage = wl_cursor_theme_get_cursor(cursorTheme, "left_ptr")->images[0];
			cursorSurface = wl_compositor_create_surface(compositor);
			wl_surface_attach(cursorSurface, wl_cursor_image_get_buffer(cursorImage), 0, 0);
			wl_surface_commit(cursorSurface);
		}
		wl_pointer_set_cursor(pointer, serial, cursorSurface,
			cursorImage->hotspot_x, cursorImage->hotspot_y);
	},
	.leave = [](void* sp, wl_pointer*, uint32_t serial, wl_surface*) {
		auto& seat = *static_cast<Seat*>(sp);
		seat.pointer->focusedBar = nullptr;
	},
	.motion = [](void* sp, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
		auto& seat = *static_cast<Seat*>(sp);
		seat.pointer->x = wl_fixed_to_int(x);
		seat.pointer->y = wl_fixed_to_int(y);
	},
	.button = [](void* sp, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t pressed) {
		auto& seat = *static_cast<Seat*>(sp);
		auto it = std::find(begin(seat.pointer->btns), end(seat.pointer->btns), button);
		if (pressed == WL_POINTER_BUTTON_STATE_PRESSED && it == end(seat.pointer->btns)) {
			seat.pointer->btns.push_back(button);
		} else if (pressed == WL_POINTER_BUTTON_STATE_RELEASED && it != end(seat.pointer->btns)) {
			seat.pointer->btns.erase(it);
		}
	},
	.axis = [](void* sp, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) { },
	.frame = [](void* sp, wl_pointer*) {
		auto& seat = *static_cast<Seat*>(sp);
		if (!seat.pointer->focusedBar) return;
		for (auto btn : seat.pointer->btns) {
			seat.pointer->focusedBar->click(seat.pointer->x, seat.pointer->y, btn);
		}
		seat.pointer->btns.clear();
	},
	.axis_source = [](void*, wl_pointer*, uint32_t) { },
	.axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) { },
	.axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) { },
};

static const struct wl_seat_listener seatListener = {
	.capabilities = [](void* sp, wl_seat*, uint32_t cap)
	{
		auto& seat = *static_cast<Seat*>(sp);
		auto hasPointer = cap & WL_SEAT_CAPABILITY_POINTER;
		if (!seat.pointer && hasPointer) {
			auto &pointer = seat.pointer.emplace();
			pointer.wlPointer = wl_unique_ptr<wl_pointer> {wl_seat_get_pointer(seat.wlSeat.get())};
			wl_pointer_add_listener(seat.pointer->wlPointer.get(), &pointerListener, &seat);
		} else if (seat.pointer && !hasPointer) {
			seat.pointer.reset();
		}
	},
	.name = [](void*, wl_seat*, const char *name) { }
};

void setupMonitor(Monitor& monitor) {
	monitor.bar.emplace(&monitor);
	monitor.bar->setStatus(lastStatus);
	auto xdgOutput = zxdg_output_manager_v1_get_xdg_output(xdgOutputManager, monitor.wlOutput.get());
	zxdg_output_v1_add_listener(xdgOutput, &xdgOutputListener, &monitor);
}

void updatemon(Monitor& mon)
{
	if (!mon.hasData) return;
	if (mon.desiredVisibility) {
		if (mon.bar->visible()) {
			mon.bar->invalidate();
		} else {
			mon.bar->show(mon.wlOutput.get());
		}
	} else if (mon.bar->visible()) {
		mon.bar->hide();
	}
}

// called after we have received the initial batch of globals
void onReady()
{
	requireGlobal(compositor, "wl_compositor");
	requireGlobal(shm, "wl_shm");
	requireGlobal(wlrLayerShell, "zwlr_layer_shell_v1");
	requireGlobal(xdgOutputManager, "zxdg_output_manager_v1");
	setupStatusFifo();
	wl_display_roundtrip(display); // roundtrip so we receive all dwl tags etc.

	ready = true;
	for (auto& monitor : monitors) {
		setupMonitor(monitor);
	}
	wl_display_roundtrip(display); // wait for xdg_output names before we read stdin
}

void setupStatusFifo()
{
	for (auto i=0; i<100; i++) {
		auto path = std::string{getenv("XDG_RUNTIME_DIR")} + "/somebar-" + std::to_string(i);
		auto result = mkfifo(path.c_str(), 0666);
		if (result == 0) {
			auto fd = open(path.c_str(), O_CLOEXEC | O_NONBLOCK | O_RDONLY);
			if (fd < 0) {
				diesys("open status fifo reader");
			}
			statusFifoName = path;
			statusFifoFd = fd;

			fd = open(path.c_str(), O_CLOEXEC | O_WRONLY);
			if (fd < 0) {
				diesys("open status fifo writer");
			}
			statusFifoWriter = fd;

			epoll_event ev = {0};
			ev.events = EPOLLIN;
			ev.data.fd = statusFifoFd;
			if (epoll_ctl(epoll, EPOLL_CTL_ADD, statusFifoFd, &ev) < 0) {
				diesys("epoll_ctl add status fifo");
			}
			return;
		} else if (errno != EEXIST) {
			diesys("mkfifo");
		}
	}
}

static LineBuffer<512> _stdinBuffer;
static void onStdin()
{
	auto res = _stdinBuffer.readLines(
		[](void* p, size_t size) { return read(0, p, size); },
		[](char* p, size_t size) { handleStdin({p, size}); });
	if (res == 0) {
		quitting = true;
	}
}

static void handleStdin(const std::string& line)
{
	// this parses the lines that dwl sends in printstatus()
	std::string monName, command;
	auto stream = std::istringstream {line};
	stream >> monName >> command;
	if (!stream.good()) {
		return;
	}
	auto mon = std::find_if(begin(monitors), end(monitors), [&](const Monitor& mon) {
		return mon.xdgName == monName;
	});
	if (mon == end(monitors))
		return;
	if (command == "title") {
		auto title = std::string {};
 		std::getline(stream, title);
		mon->bar->setTitle(title);
	} else if (command == "selmon") {
		uint32_t selected;
		stream >> selected;
		mon->bar->setSelected(selected);
		if (selected) {
			selmon = &*mon;
		} else if (selmon == &*mon) {
			selmon = nullptr;
		}
	} else if (command == "tags") {
		uint32_t occupied, tags, clientTags, urgent;
		stream >> occupied >> tags >> clientTags >> urgent;
		for (auto i=0u; i<tagNames.size(); i++) {
			auto tagMask = 1 << i;
			int state = TagState::None;
			if (tags & tagMask)
				state |= TagState::Active;
			if (urgent & tagMask)
				state |= TagState::Urgent;
			mon->bar->setTag(i, state, occupied & tagMask ? 1 : 0, clientTags & tagMask ? 0 : -1);
		}
		mon->tags = tags;
	} else if (command == "layout") {
		auto layout = std::string {};
		std::getline(stream, layout);
		mon->bar->setLayout(layout);
	}
	mon->hasData = true;
	updatemon(*mon);
}

const std::string prefixStatus = "status ";
const std::string prefixShow = "show ";
const std::string prefixHide = "hide ";
const std::string prefixToggle = "toggle ";
const std::string argAll = "all";
const std::string argSelected = "selected";

static LineBuffer<512> _statusBuffer;
void onStatus()
{
	_statusBuffer.readLines(
	[](void* p, size_t size) {
		return read(statusFifoFd, p, size);
	},
	[](const char* buffer, size_t n) {
		auto str = std::string {buffer, n};
		if (str.rfind(prefixStatus, 0) == 0) {
			lastStatus = str.substr(prefixStatus.size());
			for (auto &monitor : monitors) {
				if (monitor.bar) {
					monitor.bar->setStatus(lastStatus);
					monitor.bar->invalidate();
				}
			}
		} else if (str.rfind(prefixShow, 0) == 0) {
			updateVisibility(str.substr(prefixShow.size()), [](bool) { return true; });
		} else if (str.rfind(prefixHide, 0) == 0) {
			updateVisibility(str.substr(prefixHide.size()), [](bool) { return false; });
		} else if (str.rfind(prefixToggle, 0) == 0) {
			updateVisibility(str.substr(prefixToggle.size()), [](bool vis) { return !vis; });
		}
	});
}

void updateVisibility(const std::string& name, bool(*updater)(bool))
{
	auto isCurrent = name == argSelected;
	auto isAll = name == argAll;
	for (auto& mon : monitors) {
		if (isAll ||
			isCurrent && &mon == selmon ||
			mon.xdgName == name) {
			auto newVisibility = updater(mon.desiredVisibility);
			if (newVisibility != mon.desiredVisibility) {
				mon.desiredVisibility = newVisibility;
				updatemon(mon);
			}
		}
	}
}

struct HandleGlobalHelper {
	wl_registry* registry;
	uint32_t name;
	const char* interface;

	template<typename T>
	bool handle(T& store, const wl_interface& iface, int version) {
		if (strcmp(interface, iface.name)) return false;
		store = static_cast<T>(wl_registry_bind(registry, name, &iface, version));
		return true;
	}
};
void onGlobalAdd(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
	auto reg = HandleGlobalHelper { registry, name, interface };
	if (reg.handle(compositor, wl_compositor_interface, 4)) return;
	if (reg.handle(shm, wl_shm_interface, 1)) return;
	if (reg.handle(wlrLayerShell, zwlr_layer_shell_v1_interface, 4)) return;
	if (reg.handle(xdgOutputManager, zxdg_output_manager_v1_interface, 3)) return;
	if (reg.handle(xdgWmBase, xdg_wm_base_interface, 2)) {
		xdg_wm_base_add_listener(xdgWmBase, &xdgWmBaseListener, nullptr);
		return;
	}
	if (wl_seat *wlSeat; reg.handle(wlSeat, wl_seat_interface, 7)) {
		auto& seat = seats.emplace_back(Seat {name, wl_unique_ptr<wl_seat> {wlSeat}});
		wl_seat_add_listener(wlSeat, &seatListener, &seat);
		return;
	}
	if (wl_output *output; reg.handle(output, wl_output_interface, 1)) {
		auto& m = monitors.emplace_back(Monitor {name, {}, wl_unique_ptr<wl_output> {output}});
		if (ready) {
			setupMonitor(m);
		}
		return;
	}
}
void onGlobalRemove(void*, wl_registry* registry, uint32_t name)
{
	monitors.remove_if([name](const Monitor &mon) { return mon.registryName == name; });
	seats.remove_if([name](const Seat &seat) { return seat.name == name; });
}
static const struct wl_registry_listener registry_listener = {
	.global = onGlobalAdd,
	.global_remove = onGlobalRemove,
};

int main(int argc, char* argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, "chv")) != -1) {
		switch (opt) {
			case 'h':
				printf("Usage: %s [-h] [-v] [-c command]\n", argv[0]);
				printf("  -h: Show this help\n");
				printf("  -v: Show somebar version\n");
				printf("  -c: Sends a command to sombar. See README for details.\n");
				printf("If any of these are specified, somebar exits after the action.\n");
				printf("Otherwise, somebar will display itself.\n");
				exit(0);
			case 'v':
				printf("somebar " SOMEBAR_VERSION "\n");
				exit(0);
			case 'c':
				if (optind >= argc) {
					die("Expected command");
				}
				auto path = std::string {getenv("XDG_RUNTIME_DIR")} + "/somebar-0";
				int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
				if (fd < 0) {
					fprintf(stderr, "could not open %s: ", path.c_str());
					perror("");
					exit(1);
				}
				auto str = std::string {};
				for (auto i = optind; i<argc; i++) {
					if (i > optind) str += " ";
					str += argv[i];
				}
				str += "\n";
				write(fd, str.c_str(), str.size());
				exit(0);
		}
	}
	static sigset_t blockedsigs;
	sigemptyset(&blockedsigs);
	sigaddset(&blockedsigs, SIGINT);
	sigaddset(&blockedsigs, SIGTERM);
	sigprocmask(SIG_BLOCK, &blockedsigs, nullptr);

	epoll_event epollEv = {0};
	std::array<epoll_event, 5> epollEvents;
	epoll = epoll_create1(EPOLL_CLOEXEC);
	if (epoll < 0) {
		diesys("epoll_create1");
	}
	int sfd = signalfd(-1, &blockedsigs, SFD_CLOEXEC | SFD_NONBLOCK);
	if (sfd < 0) {
		diesys("signalfd");
	}
	epollEv.events = EPOLLIN;
	epollEv.data.fd = sfd;
	if (epoll_ctl(epoll, EPOLL_CTL_ADD, sfd, &epollEv) < 0) {
		diesys("epoll_ctl add signalfd");
	}

	display = wl_display_connect(nullptr);
	if (!display) {
		die("Failed to connect to Wayland display");
	}
	displayFd = wl_display_get_fd(display);

	auto registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, nullptr);
	wl_display_roundtrip(display);
	onReady();

	epollEv.events = EPOLLIN;
	epollEv.data.fd = displayFd;
	if (epoll_ctl(epoll, EPOLL_CTL_ADD, displayFd, &epollEv) < 0) {
		diesys("epoll_ctl add wayland_display");
	}

	epollEv.events = EPOLLIN;
	epollEv.data.fd = STDIN_FILENO;
	if (epoll_ctl(epoll, EPOLL_CTL_ADD, STDIN_FILENO, &epollEv) < 0) {
		diesys("epoll_ctl add stdin");
	}
	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

	while (!quitting) {
		waylandFlush();
		auto res = epoll_wait(epoll, epollEvents.data(), epollEvents.size(), -1);
		if (res < 0) {
			if (errno != EINTR) {
				diesys("epoll_wait");
			}
		} else {
			for (auto i=0; i<res; i++) {
				auto &ev = epollEvents[i];
				if (ev.data.fd == displayFd) {
					if (ev.events & EPOLLIN) {
						if (wl_display_dispatch(display) < 0) {
							die("wl_display_dispatch");
						}
					} if (ev.events & EPOLLOUT) {
						epollEv.events = EPOLLIN;
						epollEv.data.fd = displayFd;
						if (epoll_ctl(epoll, EPOLL_CTL_MOD, displayFd, &epollEv) < 0) {
							diesys("epoll_ctl");
						}
						waylandFlush();
					}
				} else if (ev.data.fd == STDIN_FILENO) {
					onStdin();
				} else if (ev.data.fd == statusFifoFd) {
					onStatus();
				} else if (ev.data.fd == sfd) {
					quitting = true;
				}
			}
		}
	}
	cleanup();
}

void requireGlobal(const void *p, const char *name)
{
	if (p) return;
	fprintf(stderr, "Wayland compositor does not export required global %s, aborting.\n", name);
	cleanup();
	exit(1);
}

void waylandFlush()
{
	wl_display_dispatch_pending(display);
	if (wl_display_flush(display) < 0 && errno == EAGAIN) {
		epoll_event ev = {0};
		ev.events = EPOLLIN | EPOLLOUT;
		ev.data.fd = displayFd;
		if (epoll_ctl(epoll, EPOLL_CTL_MOD, displayFd, &ev) < 0) {
			diesys("epoll_ctl");
		}
	}
}

void cleanup() {
	if (!statusFifoName.empty()) {
		unlink(statusFifoName.c_str());
	}
}

void die(const char* why) {
	fprintf(stderr, "%s\n", why);
	cleanup();
	exit(1);
}

void diesys(const char* why) {
	perror(why);
	cleanup();
	exit(1);
}
