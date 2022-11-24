# evdevgrab

Small tool that grabs a set of evdev devices and print events to stdout. The prorgam assumes appropriate permissions to access evdev devices in `/dev/input`.

## Usage
```bash
Usage: evdevgrab [OPTION...] [DEVICE]..
Grab evdev device(s) and print events to stdout. DEVICE is the path to an evdev
device.

  -n, --no-grab              No grab of devices
  -v, --verbose              Produce verbose output
  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version

Examples:
	evdevgrab /dev/input/event2 /dev/input/event3
	evdevgrab -v -n /dev/input/evdevgrab

Report bugs to /dev/null.
```
