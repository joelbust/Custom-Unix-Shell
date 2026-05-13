CC = gcc
CFLAGS = -Wall -std=c11 -pedantic -O2
TARGET = fsh
SRC = fsh.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

demo: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o

.PHONY: all demo clean