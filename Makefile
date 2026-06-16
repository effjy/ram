# Makefile for building and deploying the GTK3 RAM Visualizer on Ubuntu Mate.
# Usage:
#   make              - Compile the C binary
#   sudo make install - Install globally to application menus
#   sudo make uninstall - Clean files from system layout

CC = gcc
CFLAGS = -Wall -Wextra -O2 `pkg-config --cflags gtk+-3.0`
LIBS = `pkg-config --libs gtk+-3.0` -lm

TARGET = ram-visualizer
SRC = main.c
OBJ = $(SRC:.c=.o)

# Global directories
PREFIX ?= /usr
BIN_PATH = $(DESTDIR)$(PREFIX)/bin
ICON_PATH = $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
DESKTOP_PATH = $(DESTDIR)$(PREFIX)/share/applications

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)

install: $(TARGET)
	# 1. Install compiled binary
	install -d $(BIN_PATH)
	install -m 755 $(TARGET) $(BIN_PATH)/$(TARGET)

	# 2. Install scalable vector SVG icon
	install -d $(ICON_PATH)
	install -m 644 ram-visualizer.svg $(ICON_PATH)/$(TARGET).svg

	# 3. Install desktop launcher
	install -d $(DESKTOP_PATH)
	install -m 644 ram_visualizer.desktop $(DESKTOP_PATH)/$(TARGET).desktop

	# 4. Trigger system refreshes for responsive UI updates
	gtk-update-icon-cache -f -t $(PREFIX)/share/icons/hicolor || true
	update-desktop-database $(PREFIX)/share/applications || true
	@echo "============================================="
	@echo " RAM Visualizer installed successfully!"
	@echo " Search for 'RAM Visualizer' in the Mate menus."
	@echo "============================================="

uninstall:
	# 1. Clean executable binary
	rm -f $(BIN_PATH)/$(TARGET)

	# 2. Clean scalable SVG icon
	rm -f $(ICON_PATH)/$(TARGET).svg

	# 3. Clean desktop launcher
	rm -f $(DESKTOP_PATH)/$(TARGET).desktop

	# 4. Regenerate desktop schemas
	gtk-update-icon-cache -f -t $(PREFIX)/share/icons/hicolor || true
	update-desktop-database $(PREFIX)/share/applications || true
	@echo "============================================="
	@echo " RAM Visualizer completely uninstalled."
	@echo "============================================="

.PHONY: all clean install uninstall
