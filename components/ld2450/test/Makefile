UNITY_SRC = /opt/esp-idf/components/unity/unity/src
INCLUDES  = -I$(UNITY_SRC) -I../include
SRCS      = test_ld2450_zone.c ../ld2450_zone.c $(UNITY_SRC)/unity.c
BIN       = test_ld2450_zone

CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 $(INCLUDES)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(BIN)

.PHONY: clean
