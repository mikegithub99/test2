CXX ?= g++
TARGET := RedisJobStatusCleanup
SOURCES := RedisJobStatusCleanup.cc

CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pedantic
CPPFLAGS += $(shell pkg-config --cflags redis++ 2>/dev/null)
LDLIBS += $(shell pkg-config --libs redis++ 2>/dev/null)

ifeq ($(strip $(LDLIBS)),)
LDLIBS := -lredis++ -lhiredis
endif

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET) *.o
