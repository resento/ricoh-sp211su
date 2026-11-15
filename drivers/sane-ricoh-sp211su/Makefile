CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -fPIC
LDFLAGS ?= -shared -Wl,-soname,libsane-ricoh_sp211su.so.1 -lusb-1.0
DESTDIR ?= /usr/lib/x86_64-linux-gnu/sane
TARGET = libsane-ricoh_sp211su.so.1.0.0
SONAME = libsane-ricoh_sp211su.so.1

all: $(TARGET)

$(TARGET): ricoh_sp211su.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -d $(DESTDIR)
	install -m 0644 $(TARGET) $(DESTDIR)/$(TARGET)
	ln -sf $(TARGET) $(DESTDIR)/$(SONAME)
	ln -sf $(TARGET) $(DESTDIR)/libsane-ricoh_sp211su.so
	ldconfig

clean:
	rm -f $(TARGET)

.PHONY: all install clean
