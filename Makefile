
build:
	gcc -g evdevgrab.c -I/usr/include/libevdev-1.0 -levdev -o evdevgrab

run: build
	./evdevgrab

debug: build
	gdb evdevgrab

