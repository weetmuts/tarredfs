
$(shell mkdir -p build)

LIBTAR_A:=libtar/lib/.libs/libtar.a
LIBTAR_SOURCES:=$(shell find libtar -name "*.c" -o -name "*.h")

CXXFLAGS := -O0 -g  -Wall -fmessage-length=0 -std=c++11 -Wno-unused-function \
	"-DTARREDFS_VERSION=\"0.1\"" -DFUSE_USE_VERSION=26 \
        -Ilibtar/lib -Ilibtar/listhash -I/usr/include \
        `pkg-config fuse --cflags` 

HEADERS := $(wildcard *.h)

build/%.o: %.cc $(HEADERS)
	g++ $(CXXFLAGS) $< -c -o $@

TARREDFS_OBJS := build/util.o build/log.o build/tarfile.o build/tarentry.o build/forward.o  build/reverse.o build/main.o
DIFF_OBJS := build/util.o build/log.o build/diff.o 

LIBS =

all: build/tarredfs build/diff \
	build/tarredfs-untar build/tarredfs-pack \
	build/tarredfs-compare build/tarredfs-integrity-test

build/tarredfs: $(TARREDFS_OBJS) $(LIBTAR_A)
	$(CXX) -o build/tarredfs $(TARREDFS_OBJS) $(LIBS) $(LIBTAR_A) \
	`pkg-config fuse --libs`  `pkg-config openssl --libs` `pkg-config zlib --libs`

build/diff: $(DIFF_OBJS)
	$(CXX) -o build/diff $(DIFF_OBJS) $(LIBS) `pkg-config zlib --libs`

build/tarredfs-untar: untar.sh
	cp untar.sh build/tarredfs-untar
	chmod a+x build/tarredfs-untar

build/tarredfs-pack: pack.sh
	cp pack.sh build/tarredfs-pack
	chmod a+x build/tarredfs-pack

build/tarredfs-compare: compare.sh
	cp compare.sh build/tarredfs-compare
	chmod a+x build/tarredfs-compare

build/tarredfs-integrity-test: integrity-test.sh
	cp integrity-test.sh build/tarredfs-integrity-test
	chmod a+x build/tarredfs-integrity-test

$(LIBTAR_A): $(LIBTAR_SOURCES)
	(cd libtar; autoreconf --force --install; ./configure ; make)

install:
	echo Installing into /usr/local/bin
	cp build/tarredfs /usr/local/bin
	cp build/tarredfs-* /usr/local/bin
	mkdir -p /usr/local/lib/tarredfs
	cp format_find.pl /usr/local/lib/tarredfs/format_find.pl
	cp format_tar.pl /usr/local/lib/tarredfs/format_tar.pl
	cp tarredfs.1 /usr/local/share/man/man1

clean-all:
	(cd libtar; make clean)
	rm -rf build/* *~

clean:
	rm -f $(TARREDFS_OBJS) $(DIFF_OBJS) build/tarredfs build/diff \
	build/tarredfs-untar build/tarredfs-pack \
	build/tarredfs-compare build/tarredfs-integrity-test *~

.PHONY: clean clean-all
