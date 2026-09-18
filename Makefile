CC = gcc
CFLAGS = -Wall -Wextra -g `pkg-config --cflags gtk+-3.0 libfprint-2`
LDLIBS = `pkg-config --libs gtk+-3.0 libfprint-2`
TARGET = fprint-demo-gtk

all: $(TARGET)

$(TARGET): main.o
	$(CC) $(CFLAGS) -g -o $@ $^ $(LDLIBS)

main.o: main.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) main.o
